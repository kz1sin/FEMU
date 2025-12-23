#!/bin/bash
# Huaicheng Li <huaicheng@cs.uchicago.edu>
# Run FEMU as a black-box SSD (FTL managed by the device)

tracediskGB=$6

worker=$1
tracefile=$2
pwl=$3
superl=$4
rain_stripe_size=$5
logfile=$(printf "logworker%s" $worker)
tracepath="/home/ubuntu/share/alibabatrace/alibaba_block_traces_2020/"

# image directory
IMGDIR=/home/ubuntu/share/imageFEMU
# Virtual machine disk image
OSIMGF=$IMGDIR/u20s.qcow2

# Configurable SSD Controller layout parameters (must be power of 2)
secsz=512 # sector size in bytes
secs_per_pg=8 # number of sectors in a flash page
pgs_per_blk=1024 # number of pages per flash block
blks_per_pl=$((tracediskGB * rain_stripe_size * 8 * 21 / 5 / (rain_stripe_size - 1))) # number of blocks per plane
pls_per_lun=1 # keep it at one, no multiplanes support
luns_per_ch=1 # number of chips per channel
nchs=8 # number of channels
ssd_size=$((tracediskGB * 1024)) # in megabytes, if you change the above layout parameters, make sure you manually recalculate the ssd size and modify it here, please consider a default 25% overprovisioning ratio.

# Latency in nanoseconds
pg_rd_lat=40000 # page read latency
pg_wr_lat=200000 # page write latency
blk_er_lat=2000000 # block erase latency
ch_xfer_lat=0 # channel transfer time, ignored for now

# GC Threshold (1-100)
gc_thres_pcent=$((((rain_stripe_size - 1) * 100 - 1) / rain_stripe_size))
gc_thres_pcent_rain=97
gc_thres_pcent_high=97

#-----------------------------------------------------------------------

#Compose the entire FEMU BBSSD command line options
FEMU_OPTIONS="-device femu"
FEMU_OPTIONS=${FEMU_OPTIONS}",devsz_mb=${ssd_size}"
FEMU_OPTIONS=${FEMU_OPTIONS}",namespaces=1"
FEMU_OPTIONS=${FEMU_OPTIONS}",femu_mode=1"
FEMU_OPTIONS=${FEMU_OPTIONS}",secsz=${secsz}"
FEMU_OPTIONS=${FEMU_OPTIONS}",secs_per_pg=${secs_per_pg}"
FEMU_OPTIONS=${FEMU_OPTIONS}",pgs_per_blk=${pgs_per_blk}"
FEMU_OPTIONS=${FEMU_OPTIONS}",blks_per_pl=${blks_per_pl}"
FEMU_OPTIONS=${FEMU_OPTIONS}",pls_per_lun=${pls_per_lun}"
FEMU_OPTIONS=${FEMU_OPTIONS}",luns_per_ch=${luns_per_ch}"
FEMU_OPTIONS=${FEMU_OPTIONS}",nchs=${nchs}"
FEMU_OPTIONS=${FEMU_OPTIONS}",pg_rd_lat=${pg_rd_lat}"
FEMU_OPTIONS=${FEMU_OPTIONS}",pg_wr_lat=${pg_wr_lat}"
FEMU_OPTIONS=${FEMU_OPTIONS}",blk_er_lat=${blk_er_lat}"
FEMU_OPTIONS=${FEMU_OPTIONS}",ch_xfer_lat=${ch_xfer_lat}"
FEMU_OPTIONS=${FEMU_OPTIONS}",gc_thres_pcent=${gc_thres_pcent}"
FEMU_OPTIONS=${FEMU_OPTIONS}",gc_thres_pcent_rain=${gc_thres_pcent_rain}"
FEMU_OPTIONS=${FEMU_OPTIONS}",gc_thres_pcent_high=${gc_thres_pcent_high}"
FEMU_OPTIONS=${FEMU_OPTIONS}",rain_stripe_size=${rain_stripe_size}"
FEMU_OPTIONS=${FEMU_OPTIONS}",tracediskGB=${tracediskGB}"
FEMU_OPTIONS=${FEMU_OPTIONS}",tracefile=${tracefile}"
FEMU_OPTIONS=${FEMU_OPTIONS}",pwl=${pwl}"
FEMU_OPTIONS=${FEMU_OPTIONS}",superl=${superl}"
FEMU_OPTIONS=${FEMU_OPTIONS}",tracepath=${tracepath}"

echo ${FEMU_OPTIONS} | tee $logfile

if [[ ! -e "$OSIMGF" ]]; then
	echo ""
	echo "VM disk image couldn't be found ..."
	echo "Please prepare a usable VM image and place it as $OSIMGF"
	echo "Once VM disk image is ready, please rerun this script again"
	echo ""
	exit
fi

./qemu-system-x86_64 \
    -name "FEMU-BBSSD-VM" \
    -enable-kvm \
    -cpu host \
    -smp 4 \
    -m 4G \
    -device virtio-scsi-pci,id=scsi0 \
    -device scsi-hd,drive=hd0 \
    -drive file=$OSIMGF,if=none,aio=native,cache=none,format=qcow2,id=hd0 \
    ${FEMU_OPTIONS} \
    -net user,hostfwd=tcp::8080-:22 \
    -net nic,model=virtio \
    -virtfs local,path=/home/ubuntu/share/FEMUTest/FEMU/build-femu/fioeval,mount_tag=host0,security_model=passthrough,id=host0 \
    -device virtio-9p-pci,fsdev=host0,mount_tag=hostshare \
    -nographic \
    -qmp unix:./qmp-sock,server,nowait 2>&1 | tee -a $logfile