Overview
--------

Mongoose is easy to use web server. It also can be used as embedded web server
library to provide web interface to applications. Mongoose executable does not
depend on any external library or configuration. If it is copied to any directory
and launched from there, it starts to serve that directory on port 8080 (so to access
files, go to http://localhost:8080). If some additional config is required - for
example, different listening port or IP-based access control, that can be done via
command line flags or configuration file, which should be created in the same
directory where mongoose itself lives. This makes Mongoose perfect for all sorts
of demos, quick tests, file sharing, and Web programming.
See [manual](https://github.com/valenok/mongoose/wiki/Manual) for more information.


Features
--------

- Crossplatform - works on Windows, MacOS and most flavors of UNIX
- CGI, SSL, SSI, Digest (MD5) authorization, Websocket, WEbDAV support
- Lua server pages (like PHP, but using Lua as a language instead of PHP)
- Resumed download, URL rewrite, IP-based ACL, Windows service
- Excluding files from serving by URI pattern (file blacklist)
- Download speed limit based on client subnet or URI pattern
- Small footprint: executable size is 50 kB on Linux 2.6 i386 system
- 130 kilobytes Windows executable with all of the above compiled in, and no dependencies
- Embeddable with simple and clean API ([mongoose.h](https://github.com/valenok/mongoose/blob/master/mongoose.h)). The source is in single [mongoose.c](https://github.com/valenok/mongoose/blob/master/mongoose.c) file to make things easy.
- Embedding examples: [hello.c](https://github.com/valenok/mongoose/blob/master/examples/hello.c), [post.c](https://github.com/valenok/mongoose/blob/master/examples/post.c), [upload.c](https://github.com/valenok/mongoose/blob/master/examples/upload.c), [websocket.c](https://github.com/valenok/mongoose/blob/master/examples/websocket.c)
- Super-liberal MIT license, great for commercial users!


Mailing list
------------

You can read it online, subscribe to, or send a message at [mongoose-users](http://groups.google.com/group/mongoose-users).


Keep Sergey happy
-----------------

I have a [books wishlist](http://amzn.com/w/1OC2ZCPTQYIEP?sort=priority) on Amazon. If you feel brave, you can buy me a book!
Many thanks to all who already did so: T.Barmann, D.Hughes, J.C.Sloan and 4 others. Appreciated, guys, you keep my brains going!
