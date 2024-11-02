ssh -tt wangyuansen@10.77.110.158 'cd SepHash/build && ../ser_cli.sh 1 24 2 3' &
ssh -tt wangyuansen@10.77.110.160 'cd SepHash/build && ../ser_cli.sh 2 24 2 3' &
cd build && ../ser_cli.sh 0 24 2 3