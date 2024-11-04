ssh -tt wangyuansen@10.77.110.158 'cd SepHash/build && ../ser_cli.sh server' &
sleep 5
ssh -tt wangyuansen@10.77.110.160 'cd SepHash/build && ../ser_cli.sh server' &
sleep 5
cd build && ./ser_cli.sh server