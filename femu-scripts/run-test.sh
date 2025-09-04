cleanup() {
    echo "start kill"
    pkill qemu-system-*
    echo "end kill"
    exit 0
}

trap cleanup INT

pwl=(0 50)
superl=(-1 0 1)
rain_stripe_size=(4 8)
worker=1

# ECC 100GB
for ((p=0; p<2; p++)); do
    for ((r=0; r<2; r++)); do
        for ((t=1; t<=6; t++)); do
            bash run-markout100GBdisk.sh $worker $t ${pwl[$p]} ${superl[0]} ${rain_stripe_size[$r]} &
            ((worker++))
            echo "$worker"
            sleep 20
        done
    done
done

# PPNRAIN 100GB
for ((p=0; p<2; p++)); do
    for ((r=0; r<2; r++)); do
        for ((t=1; t<=6; t++)); do
            bash run-ppnrain100GBdisk.sh $worker $t ${pwl[$p]} ${superl[0]} ${rain_stripe_size[$r]} &
            ((worker++))
            echo "$worker"
            sleep 20
        done
    done
done

# Reforge 100GB
for ((p=0; p<2; p++)); do
    for ((s=0; s<3; s++)); do
        for ((r=0; r<2; r++)); do
            for ((t=1; t<=6; t++)); do
                bash run-reforge100GBdisk.sh $worker $t ${pwl[$p]} ${superl[$s]} ${rain_stripe_size[$r]} &
                ((worker++))
                echo "$worker"
                sleep 20
            done
        done
    done
done

# ECC 200GB
for ((p=0; p<2; p++)); do
    for ((r=0; r<2; r++)); do
        for ((t=1; t<=6; t++)); do
            bash run-markout200GBdisk.sh $worker $t ${pwl[$p]} ${superl[0]} ${rain_stripe_size[$r]} &
            ((worker++))
            echo "$worker"
            sleep 20
        done
    done
done

# PPNRAIN 200GB
for ((p=0; p<2; p++)); do
    for ((r=0; r<2; r++)); do
        for ((t=1; t<=6; t++)); do
            bash run-ppnrain200GBdisk.sh $worker $t ${pwl[$p]} ${superl[0]} ${rain_stripe_size[$r]} &
            ((worker++))
            echo "$worker"
            sleep 20
        done
    done
done

# Reforge 200GB
for ((p=0; p<2; p++)); do
    for ((s=0; s<3; s++)); do
        for ((r=0; r<2; r++)); do
            for ((t=1; t<=6; t++)); do
                bash run-reforge200GBdisk.sh $worker $t ${pwl[$p]} ${superl[$s]} ${rain_stripe_size[$r]} &
                ((worker++))
                echo "$worker"
                sleep 20
            done
        done
    done
done

echo $worker

wait