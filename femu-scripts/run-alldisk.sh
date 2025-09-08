cleanup() {
    echo "killed"
    exit 1
}

trap cleanup SIGTERM

size=$6

worker=$1
tracefile=$2
pwl=$3
superl=(1 0 -1)
rain_stripe_size=$5

slen=${#superl[@]}

echo "run-markoutdisk.sh $worker $tracefile $pwl ${superl[0]} $rain_stripe_size $size" >> logall${size}GB
bash run-markoutdisk.sh $worker $tracefile $pwl ${superl[0]} $rain_stripe_size $size &
wait

echo "run-ppnraindisk.sh $worker $tracefile $pwl ${superl[0]} $rain_stripe_size $size" >> logall${size}GB
bash run-ppnraindisk.sh $worker $tracefile $pwl ${superl[0]} $rain_stripe_size $size &
wait

for ((s=0; s<$slen; s++)); do
    echo "run-reforgedisk.sh $worker $tracefile $pwl ${superl[$s]} $rain_stripe_size $size" >> logall${size}GB
    bash run-reforgedisk.sh $worker $tracefile $pwl ${superl[$s]} $rain_stripe_size $size &
    wait
done