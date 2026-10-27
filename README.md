android:
```sh
sh build.sh
./a.out
```


ubuntu:

chroot:
```bash
gcc  clien.cpp 
sudo ./a.out 192.168.100.2
```
or
```bash
sudo socat PTY,link=/tmp/vttyHCI,mode=777,raw,echo=0 TCP:192.168.100.2:9002 &
sudo btattach -B /tmp/vttyHCI -P h4 -S 115200 &
hciconfig -a
```
zhuyi ip
