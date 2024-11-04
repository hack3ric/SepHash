ssh -tt wangyuansen@10.77.110.158 'cd SepHash/build && ../ser_cli.sh 1 8 1 3' &
sleep 3
ssh -tt wangyuansen@10.77.110.160 'cd SepHash/build && ../ser_cli.sh 2 8 1 3' &
sleep 3
cd build && ../ser_cli.sh 0 8 1 3