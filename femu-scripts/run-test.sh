cleanup() {
    echo "start kill"
    pkill -e qemu
    echo "end kill"
    exit 0
}

trap cleanup INT

pwl=(0 50)
superl=(-1 0 1)
rain_stripe_size=(4 8)
worker=1

for ((r=0; r<2; r++)); do
    for ((t=1; t<=6; t++)); do
        for ((p=0; p<2; p++)); do
            bash run-markout100GBdisk.sh $worker $t ${pwl[$p]} ${superl[0]} ${rain_stripe_size[$r]} &
            echo "run-markout100GBdisk.sh $worker $t ${pwl[$p]} ${superl[0]} ${rain_stripe_size[$r]}"
            ((worker++))
            sleep 3

            bash run-ppnrain100GBdisk.sh $worker $t ${pwl[$p]} ${superl[0]} ${rain_stripe_size[$r]} &
            echo "run-ppnrain100GBdisk.sh $worker $t ${pwl[$p]} ${superl[0]} ${rain_stripe_size[$r]}"
            ((worker++))
            sleep 3

            for ((s=0; s<3; s++)); do
                bash run-reforge100GBdisk.sh $worker $t ${pwl[$p]} ${superl[$s]} ${rain_stripe_size[$r]} &
                echo "run-reforge100GBdisk.sh $worker $t ${pwl[$p]} ${superl[$s]} ${rain_stripe_size[$r]}"
                ((worker++))
                sleep 3
            done
        done
    done
done

for ((r=0; r<2; r++)); do
    for ((t=1; t<=6; t++)); do
        for ((p=0; p<2; p++)); do
            bash run-markout200GBdisk.sh $worker $t ${pwl[$p]} ${superl[0]} ${rain_stripe_size[$r]} &
            echo "run-markout200GBdisk.sh $worker $t ${pwl[$p]} ${superl[0]} ${rain_stripe_size[$r]}"
            ((worker++))
            sleep 3

            bash run-ppnrain200GBdisk.sh $worker $t ${pwl[$p]} ${superl[0]} ${rain_stripe_size[$r]} &
            echo "run-ppnrain200GBdisk.sh $worker $t ${pwl[$p]} ${superl[0]} ${rain_stripe_size[$r]}"
            ((worker++))
            sleep 3

            for ((s=0; s<3; s++)); do
                bash run-reforge200GBdisk.sh $worker $t ${pwl[$p]} ${superl[$s]} ${rain_stripe_size[$r]} &
                echo "run-reforge200GBdisk.sh $worker $t ${pwl[$p]} ${superl[$s]} ${rain_stripe_size[$r]}"
                ((worker++))
                sleep 3
            done
        done
    done
done

wait