export HOME=/data/data/com.termux/files/home
#echo "# android_bluetooth_tcp_ubuntu" >> README.md
git init
git add .
git commit -m "first commit"
git branch -M main
git remote add origin https://github.com/wode2016501/android_bluetooth_tcp_ubuntu.git
git push -u origin main
exit
git remote add origin https://github.com/wode2016501/android_bluetooth_tcp_ubuntu.git
git branch -M main
echo "export https_proxy=socks://127.0.0.1:10808"
git push -u origin main

