# On the phone (hdc shell sh /data/local/tmp/spin_test.sh <hex cpu mask or 0> <seconds>): keep one core busy with a
# shell loop for a while (the cluster's frequency follows its busiest core), then stop it. Mask 0: no spinner, only wait.
# Prints the frequencies SP_daemon reads halfway.
m=$1; s=$2
if [ "$m" != "0" ]; then
  taskset $m sh -c 'while :; do :; done' &
  p=$!
fi
sleep $((s / 2))
SP_daemon -N 2 -c | grep -o 'cpu[0-9]*Frequency=[0-9]*' | sort -u | head -40
sleep $((s / 2))
[ -n "$p" ] && kill $p
echo "spinner mask $m done"
