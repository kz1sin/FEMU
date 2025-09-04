# Configurable SSD Controller layout parameters (must be power of 2)
secsz=512 # sector size in bytes
secs_per_pg=8 # number of sectors in a flash page
pgs_per_blk=1024 # number of pages per flash block
blks_per_pl=17920 # number of blocks per plane
pls_per_lun=1 # keep it at one, no multiplanes support
luns_per_ch=1 # number of chips per channel
nchs=4 # number of channels
ssd_size=204800 # in megabytes, if you change the above layout parameters, make sure you manually recalculate the ssd size and modify it here, please consider a default 25% overprovisioning ratio.

# Latency in nanoseconds
pg_rd_lat=4 # page read latency
pg_wr_lat=20 # page write latency
blk_er_lat=20 # block erase latency
ch_xfer_lat=0 # channel transfer time, ignored for now

# GC Threshold (1-100)
gc_thres_pcent=74
gc_thres_pcent_rain=97
gc_thres_pcent_high=98

tracediskGB=200

worker=$1
tracefile=$2
pwl=$3
superl=$4
rain_stripe_size=$5
logfile=$(printf "logworker%s" $worker)

# test=$(printf "worker %s trace %s pwl %s superl %s size %s" $worker $tracefile $pwl $superl $rain_stripe_size)
# echo $test > $logfile

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

./qemu-system-reforge \
    -name "FEMU-BBSSD-VM" \
    -enable-kvm \
    -cpu host \
    -smp 4 \
    -m 4G \
    -device virtio-scsi-pci,id=scsi0 \
    ${FEMU_OPTIONS} \
    -nographic > $logfile 2>&1