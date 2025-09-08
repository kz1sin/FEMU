declare -a PIDS=()

cleanup() {
    for PID in "${PIDS[@]}"; do
        echo "kill $PID"
        kill $PID
    done
    wait

    echo "start kill qemu"
    pkill -e qemu
    echo "end kill"
    exit 1
}

trap cleanup SIGINT

disksizes=(200 100)
pwl=(50)
superl=(-1 0 1)
rain_stripe_size=(4 8)
worker=1

dlen=${#disksizes[@]}
plen=${#pwl[@]}
slen=${#superl[@]}
rlen=${#rain_stripe_size[@]}
traces=60
sleeptime=20

for ((d=0; d<$dlen; d++)); do
    for ((r=0; r<$rlen; r++)); do
        for ((t=1; t<=$traces; t++)); do
            for ((p=0; p<$plen; p++)); do
                echo "run-alldisk.sh $worker $t ${pwl[$p]} ${superl[0]} ${rain_stripe_size[$r]} ${disksizes[$d]}"
                bash run-alldisk.sh $worker $t ${pwl[$p]} ${superl[0]} ${rain_stripe_size[$r]} ${disksizes[$d]} &
                pid=$!
                PIDS+=($pid)
                ((worker++))
                sleep $sleeptime
            done
        done
    done
done

wait