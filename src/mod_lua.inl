#include <lua.h>
#include <lauxlib.h>
#include <setjmp.h>

#ifdef _WIN32
static void *mmap(void *addr, int64_t len, int prot, int flags, int fd,
    int offset)
{
    HANDLE fh = (HANDLE) _get_osfhandle(fd);
    HANDLE mh = CreateFileMapping(fh, 0, PAGE_READONLY, 0, 0, 0);
    void *p = MapViewOfFile(mh, FILE_MAP_READ, 0, 0, (size_t) len);
    CloseHandle(mh);
    return p;
}

static void munmap(void *addr, int64_t length)
{
    UnmapViewOfFile(addr);
}

#define MAP_FAILED NULL
#define MAP_PRIVATE 0
#define PROT_READ 0
#else
#include <sys/mman.h>
#endif

static const char *LUASOCKET = "luasocket";

/* Forward declarations */
static void handle_request(struct mg_connection *);
static int handle_lsp_request(struct mg_connection *, const char *,
struct file *, struct lua_State *);

static void reg_string(struct lua_State *L, const char *name, const char *val)
{
    if (name!=NULL && val!=NULL) {
        lua_pushstring(L, name);
        lua_pushstring(L, val);
        lua_rawset(L, -3);
    }
}

static void reg_int(struct lua_State *L, const char *name, int val)
{
    if (name!=NULL) {
        lua_pushstring(L, name);
        lua_pushinteger(L, val);
        lua_rawset(L, -3);
    }
}

static void reg_boolean(struct lua_State *L, const char *name, int val)
{
    if (name!=NULL) {
        lua_pushstring(L, name);
        lua_pushboolean(L, val != 0);
        lua_rawset(L, -3);
    }
}

static void reg_function(struct lua_State *L, const char *name,
    lua_CFunction func, struct mg_connection *conn)
{
    if (name!=NULL && func!=NULL) {
        lua_pushstring(L, name);
        lua_pushlightuserdata(L, conn);
        lua_pushcclosure(L, func, 1);
        lua_rawset(L, -3);
    }
}

static int lsp_sock_close(lua_State *L)
{
    if (lua_gettop(L) > 0 && lua_istable(L, -1)) {
        lua_getfield(L, -1, "sock");
        closesocket((SOCKET) lua_tonumber(L, -1));
    } else {
        return luaL_error(L, "invalid :close() call");
    }
    return 1;
}

static int lsp_sock_recv(lua_State *L)
{
    char buf[2000];
    int n;

    if (lua_gettop(L) > 0 && lua_istable(L, -1)) {
        lua_getfield(L, -1, "sock");
        n = recv((SOCKET) lua_tonumber(L, -1), buf, sizeof(buf), 0);
        if (n <= 0) {
            lua_pushnil(L);
        } else {
            lua_pushlstring(L, buf, n);
        }
    } else {
        return luaL_error(L, "invalid :close() call");
    }
    return 1;
}

static int lsp_sock_send(lua_State *L)
{
    const char *buf;
    size_t len, sent = 0;
    int n = 0, sock;

    if (lua_gettop(L) > 1 && lua_istable(L, -2) && lua_isstring(L, -1)) {
        buf = lua_tolstring(L, -1, &len);
        lua_getfield(L, -2, "sock");
        sock = (int) lua_tonumber(L, -1);
        while (sent < len) {
            if ((n = send(sock, buf + sent, (int)(len - sent), 0)) <= 0) {
                break;
            }
            sent += n;
        }
        lua_pushnumber(L, n);
    } else {
        return luaL_error(L, "invalid :close() call");
    }
    return 1;
}

static const struct luaL_Reg luasocket_methods[] = {
    {"close", lsp_sock_close},
    {"send", lsp_sock_send},
    {"recv", lsp_sock_recv},
    {NULL, NULL}
};

static int lsp_connect(lua_State *L)
{
    char ebuf[100];
    SOCKET sock;

    if (lua_isstring(L, -3) && lua_isnumber(L, -2) && lua_isnumber(L, -1)) {
        sock = conn2(NULL, lua_tostring(L, -3), (int) lua_tonumber(L, -2),
            (int) lua_tonumber(L, -1), ebuf, sizeof(ebuf));
        if (sock == INVALID_SOCKET) {
            return luaL_error(L, ebuf);
        } else {
            lua_newtable(L);
            reg_int(L, "sock", (int) sock);
            reg_string(L, "host", lua_tostring(L, -4));
            luaL_getmetatable(L, LUASOCKET);
            lua_setmetatable(L, -2);
        }
    } else {
        return luaL_error(L, "connect(host,port,is_ssl): invalid parameter given.");
    }
    return 1;
}

static int lsp_error(lua_State *L)
{
    lua_getglobal(L, "mg");
    lua_getfield(L, -1, "onerror");
    lua_pushvalue(L, -3);
    lua_pcall(L, 1, 0, 0);
    return 0;
}

/* Silently stop processing chunks. */
static void lsp_abort(lua_State *L)
{
    int top = lua_gettop(L);
    lua_getglobal(L, "mg");
    lua_pushnil(L);
    lua_setfield(L, -2, "onerror");
    lua_settop(L, top);
    lua_pushstring(L, "aborting");
    lua_error(L);
}

struct lsp_var_reader_data
{
    const char * begin;
    unsigned len;
    unsigned state;
};

static const char * lsp_var_reader(lua_State *L, void *ud, size_t *sz)
{
    struct lsp_var_reader_data * reader = (struct lsp_var_reader_data *)ud;
    const char * ret;

    switch (reader->state) {
    case 0:
        ret = "mg.write(";
        *sz = strlen(ret);
        break;
    case 1:
        ret = reader->begin;
        *sz = reader->len;
        break;
    case 2:
        ret = ")";
        *sz = strlen(ret);
        break;
    default:
        ret = 0;
        *sz = 0;
    }

    reader->state++;
    return ret;
}

static int lsp(struct mg_connection *conn, const char *path,
    const char *p, int64_t len, lua_State *L)
{
    int i, j, pos = 0, lines = 1, lualines = 0, is_var, lua_ok;
    char chunkname[MG_BUF_LEN];
    struct lsp_var_reader_data data;

    for (i = 0; i < len; i++) {
        if (p[i] == '\n') lines++;
        if ((i + 1) < len && p[i] == '<' && p[i + 1] == '?') {

            /* <?= ?> means a variable is enclosed and its value should be printed */
            is_var = ((i + 2) < len && p[i + 2] == '=');

            if (is_var) j = i + 2;
            else j = i + 1;

            while (j < len) {
                if (p[j] == '\n') lualines++;
                if ((j + 1) < len && p[j] == '?' && p[j + 1] == '>') {
                    mg_write(conn, p + pos, i - pos);

                    snprintf(chunkname, sizeof(chunkname), "@%s+%i", path, lines);
                    lua_pushlightuserdata(L, conn);
                    lua_pushcclosure(L, lsp_error, 1);

                    if (is_var) {
                        data.begin = p + (i + 3);
                        data.len = j - (i + 3);
                        data.state = 0;
                        lua_ok = lua_load(L, lsp_var_reader, &data, chunkname, NULL);
                    } else {
                        lua_ok = luaL_loadbuffer(L, p + (i + 2), j - (i + 2), chunkname);
                    }

                    if (lua_ok) {
                        /* Syntax error or OOM. Error message is pushed on stack. */
                        lua_pcall(L, 1, 0, 0);
                    } else {
                        /* Success loading chunk. Call it. */
                        lua_pcall(L, 0, 0, 1);
                    }

                    pos = j + 2;
                    i = pos - 1;
                    break;
                }
                j++;
            }

            if (lualines > 0) {
                lines += lualines;
                lualines = 0;
            }
        }
    }

    if (i > pos) {
        mg_write(conn, p + pos, i - pos);
    }

    return 0;
}

static int lsp_write(lua_State *L)
{
    int i, num_args;
    const char *str;
    size_t size;
    struct mg_connection *conn = lua_touserdata(L, lua_upvalueindex(1));

    num_args = lua_gettop(L);
    for (i = 1; i <= num_args; i++) {
        if (lua_isstring(L, i)) {
            str = lua_tolstring(L, i, &size);
            mg_write(conn, str, size);
        }
    }

    return 0;
}

static int lsp_read(lua_State *L)
{
    struct mg_connection *conn = lua_touserdata(L, lua_upvalueindex(1));
    char buf[1024];
    int len = mg_read(conn, buf, sizeof(buf));

    if (len <= 0) return 0;
    lua_pushlstring(L, buf, len);

    return 1;
}

/* mg.include: Include another .lp file */
static int lsp_include(lua_State *L)
{
    struct mg_connection *conn = lua_touserdata(L, lua_upvalueindex(1));
    struct file file = STRUCT_FILE_INITIALIZER;
    if (handle_lsp_request(conn, lua_tostring(L, -1), &file, L)) {
        /* handle_lsp_request returned an error code, meaning an error occured in
        the included page and mg.onerror returned non-zero. Stop processing. */
        lsp_abort(L);
    }
    return 0;
}

/* mg.cry: Log an error. Default value for mg.onerror. */
static int lsp_cry(lua_State *L)
{
    struct mg_connection *conn = lua_touserdata(L, lua_upvalueindex(1));
    mg_cry(conn, "%s", lua_tostring(L, -1));
    return 0;
}

/* mg.redirect: Redirect the request (internally). */
static int lsp_redirect(lua_State *L)
{
    struct mg_connection *conn = lua_touserdata(L, lua_upvalueindex(1));
    conn->request_info.uri = lua_tostring(L, -1);
    handle_request(conn);
    lsp_abort(L);
    return 0;
}

static int lwebsock_write(lua_State *L)
{
#ifdef USE_WEBSOCKET
    int num_args = lua_gettop(L);
    struct mg_connection *conn = lua_touserdata(L, lua_upvalueindex(1));
    const char *str;
    size_t size;
    int opcode = -1;

    if (num_args == 1) {
        if (lua_isstring(L, 1)) {
            str = lua_tolstring(L, 1, &size);
            mg_websocket_write(conn, WEBSOCKET_OPCODE_TEXT, str, size);
        }
    } else if (num_args == 2) {
        if (lua_isnumber(L, 1)) {
            opcode = (int)lua_tointeger(L, 1);
        } else if (lua_isstring(L,1)) {
            str = lua_tostring(L, 1);
            if (!mg_strncasecmp(str, "text", 4)) opcode = WEBSOCKET_OPCODE_TEXT;
            else if (!mg_strncasecmp(str, "bin", 3)) opcode = WEBSOCKET_OPCODE_BINARY;
            else if (!mg_strncasecmp(str, "close", 5)) opcode = WEBSOCKET_OPCODE_CONNECTION_CLOSE;
            else if (!mg_strncasecmp(str, "ping", 4)) opcode = WEBSOCKET_OPCODE_PING;
            else if (!mg_strncasecmp(str, "pong", 4)) opcode = WEBSOCKET_OPCODE_PONG;
            else if (!mg_strncasecmp(str, "cont", 4)) opcode = WEBSOCKET_OPCODE_CONTINUATION;
        }
        if (opcode>=0 && opcode<16 && lua_isstring(L, 2)) {
            str = lua_tolstring(L, 2, &size);
            mg_websocket_write(conn, WEBSOCKET_OPCODE_TEXT, str, size);
        }
    }
#endif
    return 0;
}

enum {
    LUA_ENV_TYPE_LUA_SERVER_PAGE = 0,
    LUA_ENV_TYPE_PLAIN_LUA_PAGE = 1,
    LUA_ENV_TYPE_LUA_WEBSOCKET = 2,
};

static void prepare_lua_environment(struct mg_connection *conn, lua_State *L, const char *script_name, int lua_env_type)
{
    const struct mg_request_info *ri = mg_get_request_info(conn);
    char src_addr[IP_ADDR_STR_LEN];
    int i;

    extern void luaL_openlibs(lua_State *);

    sockaddr_to_string(src_addr, sizeof(src_addr), &conn->client.rsa);

    luaL_openlibs(L);
#ifdef USE_LUA_SQLITE3
    {
        extern int luaopen_lsqlite3(lua_State *);
        luaopen_lsqlite3(L);
    }
#endif
#ifdef USE_LUA_FILE_SYSTEM
    {
        extern int luaopen_lfs(lua_State *);
        luaopen_lfs(L);
    }
#endif

    luaL_newmetatable(L, LUASOCKET);
    lua_pushliteral(L, "__index");
    luaL_newlib(L, luasocket_methods);
    lua_rawset(L, -3);
    lua_pop(L, 1);
    lua_register(L, "connect", lsp_connect);

    if (conn == NULL) {
        /* Do not register any connection specific functions or variables */
        return;
    }

    /* Register mg module */
    lua_newtable(L);

    reg_function(L, "cry", lsp_cry, conn);

    switch (lua_env_type) {
        case LUA_ENV_TYPE_LUA_SERVER_PAGE:
            reg_string(L, "lua_type", "page");
            break;
        case LUA_ENV_TYPE_PLAIN_LUA_PAGE:
            reg_string(L, "lua_type", "script");
            break;
        case LUA_ENV_TYPE_LUA_WEBSOCKET:
            reg_string(L, "lua_type", "websocket");
            break;
    }

    if (lua_env_type==LUA_ENV_TYPE_LUA_SERVER_PAGE || lua_env_type==LUA_ENV_TYPE_PLAIN_LUA_PAGE) {
        reg_function(L, "read", lsp_read, conn);
        reg_function(L, "write", lsp_write, conn);
    }

    if (lua_env_type==LUA_ENV_TYPE_LUA_SERVER_PAGE) {
        reg_function(L, "include", lsp_include, conn);
        reg_function(L, "redirect", lsp_redirect, conn);
    }

    if (lua_env_type==LUA_ENV_TYPE_LUA_WEBSOCKET) {
        reg_function(L, "write", lwebsock_write, conn);
    }

    reg_string(L, "version", CIVETWEB_VERSION);
    reg_string(L, "document_root", conn->ctx->config[DOCUMENT_ROOT]);
    reg_string(L, "auth_domain", conn->ctx->config[AUTHENTICATION_DOMAIN]);
#if defined(USE_WEBSOCKET)
    reg_string(L, "websocket_root", conn->ctx->config[WEBSOCKET_ROOT]);
#endif

    /* Export request_info */
    lua_pushstring(L, "request_info");
    lua_newtable(L);
    reg_string(L, "request_method", ri->request_method);
    reg_string(L, "uri", ri->uri);
    reg_string(L, "http_version", ri->http_version);
    reg_string(L, "query_string", ri->query_string);
    reg_int(L, "remote_ip", ri->remote_ip); /* remote_ip is deprecated, use remote_addr instead */
    reg_string(L, "remote_addr", src_addr);
    /* TODO: ip version */
    reg_int(L, "remote_port", ri->remote_port);
    reg_int(L, "num_headers", ri->num_headers);
    reg_int(L, "server_port", ntohs(conn->client.lsa.sin.sin_port));

    if (conn->request_info.remote_user != NULL) {
        reg_string(L, "remote_user", conn->request_info.remote_user);
        reg_string(L, "auth_type", "Digest");
    }

    lua_pushstring(L, "http_headers");
    lua_newtable(L);
    for (i = 0; i < ri->num_headers; i++) {
        reg_string(L, ri->http_headers[i].name, ri->http_headers[i].value);
    }
    lua_rawset(L, -3);

    reg_boolean(L, "https", conn->ssl != NULL);
    reg_string(L, "script_name", script_name);

    lua_rawset(L, -3);
    lua_setglobal(L, "mg");

    /* Register default mg.onerror function */
    IGNORE_UNUSED_RESULT(luaL_dostring(L, "mg.onerror = function(e) mg.write('\\nLua error:\\n', "
        "debug.traceback(e, 1)) end"));
}

static int lua_error_handler(lua_State *L)
{
    const char *error_msg =  lua_isstring(L, -1) ?  lua_tostring(L, -1) : "?\n";

    lua_getglobal(L, "mg");
    if (!lua_isnil(L, -1)) {
        lua_getfield(L, -1, "write");   /* call mg.write() */
        lua_pushstring(L, error_msg);
        lua_pushliteral(L, "\n");
        lua_call(L, 2, 0);
        IGNORE_UNUSED_RESULT(luaL_dostring(L, "mg.write(debug.traceback(), '\\n')"));
    } else {
        printf("Lua error: [%s]\n", error_msg);
        IGNORE_UNUSED_RESULT(luaL_dostring(L, "print(debug.traceback(), '\\n')"));
    }
    /* TODO(lsm): leave the stack balanced */

    return 0;
}

void mg_exec_lua_script(struct mg_connection *conn, const char *path,
    const void **exports)
{
    int i;
    lua_State *L;

    if (path != NULL && (L = luaL_newstate()) != NULL) {
        prepare_lua_environment(conn, L, path, LUA_ENV_TYPE_PLAIN_LUA_PAGE);
        lua_pushcclosure(L, &lua_error_handler, 0);

        if (exports != NULL) {
            lua_pushglobaltable(L);
            for (i = 0; exports[i] != NULL && exports[i + 1] != NULL; i += 2) {
                lua_pushstring(L, exports[i]);
                lua_pushcclosure(L, (lua_CFunction) exports[i + 1], 0);
                lua_rawset(L, -3);
            }
        }

        if (luaL_loadfile(L, path) != 0) {
            lua_error_handler(L);
        }
        lua_pcall(L, 0, 0, -2);
        lua_close(L);
    }
    conn->must_close=1;
}

static void lsp_send_err(struct mg_connection *conn, struct lua_State *L,
    const char *fmt, ...)
{
    char buf[MG_BUF_LEN];
    va_list ap;
    int len;

    va_start(ap, fmt);
    len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (L == NULL) {
        send_http_error(conn, 500, http_500_error, "%s", buf);
    } else {
        lua_pushstring(L, buf);
        lua_error(L);
    }
}

static int handle_lsp_request(struct mg_connection *conn, const char *path,
struct file *filep, struct lua_State *ls)
{
    void *p = NULL;
    lua_State *L = NULL;
    int error = 1;

    /* We need both mg_stat to get file size, and mg_fopen to get fd */
    if (!mg_stat(conn, path, filep) || !mg_fopen(conn, path, "r", filep)) {
        lsp_send_err(conn, ls, "File [%s] not found", path);
    } else if (filep->membuf == NULL &&
        (p = mmap(NULL, (size_t) filep->size, PROT_READ, MAP_PRIVATE,
        fileno(filep->fp), 0)) == MAP_FAILED) {
            lsp_send_err(conn, ls, "mmap(%s, %zu, %d): %s", path, (size_t) filep->size,
                fileno(filep->fp), strerror(errno));
    } else if ((L = ls != NULL ? ls : luaL_newstate()) == NULL) {
        send_http_error(conn, 500, http_500_error, "%s", "luaL_newstate failed");
    } else {
        /* We're not sending HTTP headers here, Lua page must do it. */
        if (ls == NULL) {
            prepare_lua_environment(conn, L, path, LUA_ENV_TYPE_LUA_SERVER_PAGE);
            if (conn->ctx->callbacks.init_lua != NULL) {
                conn->ctx->callbacks.init_lua(conn, L);
            }
        }
        error = lsp(conn, path, filep->membuf == NULL ? p : filep->membuf,
            filep->size, L);
    }

    if (L != NULL && ls == NULL) lua_close(L);
    if (p != NULL) munmap(p, filep->size);
    mg_fclose(filep);
    conn->must_close=1;
    return error;
}

#ifdef USE_WEBSOCKET
struct lua_websock_data {
    lua_State *main;
    lua_State *thread;
};

static void websock_cry(struct mg_connection *conn, int err, lua_State * L, const char * ws_operation, const char * lua_operation)
{
    switch (err) {
        case LUA_OK:
        case LUA_YIELD:
            break;
        case LUA_ERRRUN:
            mg_cry(conn, "%s: %s failed: runtime error: %s", ws_operation, lua_operation, lua_tostring(L, -1));
            break;
        case LUA_ERRSYNTAX:
            mg_cry(conn, "%s: %s failed: syntax error: %s", ws_operation, lua_operation, lua_tostring(L, -1));
            break;
        case LUA_ERRMEM:
            mg_cry(conn, "%s: %s failed: out of memory", ws_operation, lua_operation);
            break;
        case LUA_ERRGCMM:
            mg_cry(conn, "%s: %s failed: error during garbage collection", ws_operation, lua_operation);
            break;
        case LUA_ERRERR:
            mg_cry(conn, "%s: %s failed: error in error handling: %s", ws_operation, lua_operation, lua_tostring(L, -1));
            break;
        default:
            mg_cry(conn, "%s: %s failed: error %i", ws_operation, lua_operation, err);
            break;
    }
}

static void * new_lua_websocket(const char * script, struct mg_connection *conn)
{
    struct lua_websock_data *lws_data;
    int ok = 0;
    int err, nargs;

    assert(conn->lua_websocket_state == NULL);
    lws_data = (struct lua_websock_data *) malloc(sizeof(*lws_data));

    if (lws_data) {
        lws_data->main = luaL_newstate();
        if (lws_data->main) {
            prepare_lua_environment(conn, lws_data->main, script, LUA_ENV_TYPE_LUA_WEBSOCKET);
            if (conn->ctx->callbacks.init_lua != NULL) {
                conn->ctx->callbacks.init_lua(conn, lws_data->main);
            }
            lws_data->thread = lua_newthread(lws_data->main);
            err = luaL_loadfile(lws_data->thread, script);
            if (err==LUA_OK) {
                /* Activate the Lua script. */
                err = lua_resume(lws_data->thread, NULL, 0);
                if (err!=LUA_YIELD) {
                    websock_cry(conn, err, lws_data->thread, __func__, "lua_resume");
                } else {
                    nargs = lua_gettop(lws_data->thread);
                    ok = (nargs==1) && lua_isboolean(lws_data->thread, 1) && lua_toboolean(lws_data->thread, 1);
                }
            } else {
                websock_cry(conn, err, lws_data->thread, __func__, "lua_loadfile");
            }

        } else {
            mg_cry(conn, "%s: luaL_newstate failed", __func__);
        }

        if (!ok) {
            if (lws_data->main) lua_close(lws_data->main);
            free(lws_data);
            lws_data=0;
        }
    } else {
        mg_cry(conn, "%s: out of memory", __func__);
    }

    return lws_data;
}

static int lua_websocket_data(struct mg_connection *conn, int bits, char *data, size_t data_len)
{
    struct lua_websock_data *lws_data = (struct lua_websock_data *)(conn->lua_websocket_state);
    int err, nargs, ok=0, retry;
    lua_Number delay;

    assert(lws_data != NULL);
    assert(lws_data->main != NULL);
    assert(lws_data->thread != NULL);

    do {
        retry=0;

        /* Push the data to Lua, then resume the Lua state. */
        /* The data will be available to Lua as the result of the coroutine.yield function. */
        lua_pushboolean(lws_data->thread, 1);
        if (bits >= 0) {
            lua_pushinteger(lws_data->thread, bits);
            if (data) {
                lua_pushlstring(lws_data->thread, data, data_len);
                err = lua_resume(lws_data->thread, NULL, 3);
            } else {
                err = lua_resume(lws_data->thread, NULL, 2);
            }
        } else {
            err = lua_resume(lws_data->thread, NULL, 1);
        }

        /* Check if Lua returned by a call to the coroutine.yield function. */
        if (err!=LUA_YIELD) {
            websock_cry(conn, err, lws_data->thread, __func__, "lua_resume");
        } else {
            nargs = lua_gettop(lws_data->thread);
            ok = (nargs>=1) && lua_isboolean(lws_data->thread, 1) && lua_toboolean(lws_data->thread, 1);
            delay = (nargs>=2) && lua_isnumber(lws_data->thread, 2) ? lua_tonumber(lws_data->thread, 2) : -1.0;
            if (ok && delay>0) {
                fd_set rfds;
                struct timeval tv;

                FD_ZERO(&rfds);
                FD_SET(conn->client.sock, &rfds);

                tv.tv_sec = (unsigned long)delay;
                tv.tv_usec = (unsigned long)(((double)delay - (double)((unsigned long)delay))*1000000.0);
                retry = (0==select(conn->client.sock+1, &rfds, NULL, NULL, &tv));
            }
        }
    } while (retry);

    return ok;
}

static int lua_websocket_ready(struct mg_connection *conn)
{
    return lua_websocket_data(conn, -1, NULL, 0);
}

static void lua_websocket_close(struct mg_connection *conn)
{
    struct lua_websock_data *lws_data = (struct lua_websock_data *)(conn->lua_websocket_state);
    int err;

    assert(lws_data != NULL);
    assert(lws_data->main != NULL);
    assert(lws_data->thread != NULL);

    lua_pushboolean(lws_data->thread, 0);
    err = lua_resume(lws_data->thread, NULL, 1);

    lua_close(lws_data->main);
    free(lws_data);
    conn->lua_websocket_state = NULL;
}
#endif
