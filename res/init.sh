#!/bin/busybox sh

/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t devtmpfs devtmpfs /dev
/bin/busybox mount -t sysfs sysfs /sys

/bin/busybox --install -s /bin

mkdir -p /etc

cat << 'EOF' > /etc/profile
echo "========================================="
echo " Welcome to RV32 Linux (BusyBox + Lua)!"
echo " Machine: riscv-rv32ima"
echo "========================================="
EOF

echo "root:x:0:0:root:/root:/bin/sh" > /etc/passwd
echo "root:x:0:" > /etc/group
echo "rv32ima" > /etc/hostname
hostname -F /etc/hostname

export HOME=/root
export USER=root
export LOGNAME=root
export TERM=xterm-256color
export PATH=/bin:/sbin
export PS1='\[\e[1;34m\]\u@\h\[\e[0m\]:\[\e[1;32m\]\w\[\e[0m\]\$ '
stty rows 32 cols 80 2>/dev/null

cd /root
/bin/busybox setsid /bin/busybox cttyhack /bin/sh -l
echo "Session ended. Powering off..."
/bin/busybox poweroff -f
