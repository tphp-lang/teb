if not exist "..\lib\windows\x86_64" mkdir "..\lib\windows\x86_64"
clang -O2 -shared -DNO_SSL -I. ".\civetweb.c" -o "..\lib\windows\x86_64\teb.dll" -lws2_32 -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic