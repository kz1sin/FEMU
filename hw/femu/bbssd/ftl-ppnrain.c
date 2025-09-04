#include <math.h>
#include "ftl.h"

//#define FEMU_DEBUG_FTL

typedef struct LIF {
    struct line* ln;
    int erasecount;
    size_t writtenMinPos;
} LineInfo;

typedef struct StripeInfo {
    int stripeindex, vpc, ipc;
    double upersum;
    QTAILQ_ENTRY(StripeInfo) entry;
    size_t victimPos, writtenMinPos;
} StripeInfo;

// should reset
static uint64_t hostPageWrites = 0, ssdPageWrites = 0, GCPageWrites = 0, parityPageWrites = 0, PWLPageWrites = 0;
static int gcCount = 0, currStripeOffset = 0;
static double currErrorRate = 0;
static FILE* outfp;
static LineInfo* lineinfo;
static StripeInfo* stripeinfo;
pqueue_t* writtenMinPQ, * victimStripePQ; // priority queue for PWL
QTAILQ_HEAD(freeStripeList, StripeInfo) freeStripeList;
QTAILQ_HEAD(fullStripeList, StripeInfo) fullStripeList;
static int* stripe2line = NULL, * line2stripe = NULL; // mapping between line id and stripe, remains the same in this baseline case
static double* lineuper;

// remains unchanged after init
static const uint64_t PARITYLPN = INVALID_LPN - 1;
int logicalPages = 0, RAINlogicalPages = 0, totalStripes = 0, RAINCycleLimit = 0;
static double targetErrorRate = 0;
static QemuThread trace_thread;

// RBER model parameters
static const double K = 2.05;
static const double ALPHA = 3.9e-10;
static const double EPSILON = 1.48e-3;
static const int ECC = 128;
static const int CYCLELIMIT = 900;

static inline double RBER(int cycle) {
    return EPSILON + ALPHA * pow(cycle, K);
}

static double UPER(int cycle) {
    const int BITS = 4096 * 8; // assume page size 4KB
    double rber = RBER(cycle);
    // calculate probability of (error bits > ECC)
    double combin = 1;
    for (int i = 1;i <= ECC + 1;i++) {
        combin *= rber * (BITS - i + 1) / i;
    }
    double currTerm = combin * pow(1 - rber, BITS - ECC - 1);
    double pe = currTerm;
    // add up each term until error within 1%
    for (int i = ECC + 2;i <= BITS;i++) {
        double mul = rber * (BITS - i + 1) / i / (1 - rber);
        if (mul <= 0.5 && currTerm <= pe * 0.005) {
            break;
        }
        currTerm *= mul;
        pe += currTerm;
    }

    return pe;
}

static double DevicePFail(struct ssd* ssd) {
    struct ssdparams* spp = &ssd->sp;
    double p = 0, maxupersum = 0, minupersum = 0;
    int maxupersumstripe = 0, minupersumstripe = 0;
    for (int s = 0; s < totalStripes; s++) {
        int stripeindex = s * spp->rain_stripe_size;
        double minus = 0, sum = 0;
        for (int o = 0;o < spp->rain_stripe_size;o += 1) {
            int ln = stripe2line[stripeindex + o];
            sum += lineuper[ln];
            minus += lineuper[ln] * lineuper[ln];
        }
        double err = fabs((sum - stripeinfo[s].upersum) / sum);
        assert(err < 1e-10);
        double stripeuper = (sum * sum - minus) / 2.0;
        if (s == 0 || sum > maxupersum) {
            maxupersum = sum;
            maxupersumstripe = s;
        }
        if (s == 0 || sum < minupersum) {
            minupersum = sum;
            minupersumstripe = s;
        }
        p += stripeuper;
    }
    fprintf(outfp, "maxupersum %d %e minupersum %d %e\n", maxupersumstripe, maxupersum, minupersumstripe, minupersum);
    return p / spp->tt_lines * RAINlogicalPages; // scale to logical size
}

void dumpBlocks(struct ssd* ssd) {
    struct ssdparams* spp = &ssd->sp;
    currErrorRate = DevicePFail(ssd);
    fprintf(outfp, "devicep %e targetp %e\n", currErrorRate, targetErrorRate);
    if (currErrorRate >= targetErrorRate) {
        fprintf(outfp, "\nlineerasecount\n");
        // lines grouped by stripes
        for (int lnindex = 0; lnindex < spp->tt_lines; lnindex++) {
            int ln = stripe2line[lnindex];
            int cnt = lineinfo[ln].erasecount;
            fprintf(outfp, "%d ", cnt);
            if (lnindex % 16 == 15) {
                fprintf(outfp, "\n");
                fflush(outfp);
            }
        }
        // int freecount = 0;
        // fprintf(outfp, "\nfreelines\n");
        // for (int b = 0; b < spp->tt_lines; b++) {
        //     struct line* l = &ssd->lm.lines[b];
        //     int free = (l->ipc == 0) && (l->vpc == 0);
        //     freecount += free;
        //     fprintf(outfp, "%d ", free);
        //     if (b % 16 == 15) {
        //         fprintf(outfp, "\n");
        //         fflush(outfp);
        //     }
        // }
        // fprintf(outfp, "\nfreecount %d\n", freecount);
    }
}

static void* ftl_thread(void* arg);
static void* trace(void* arg);

static inline bool should_gc(struct ssd* ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines_rain);
}

static inline bool should_gc_high(struct ssd* ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines_high);
}

static inline struct ppa get_maptbl_ent(struct ssd* ssd, uint64_t lpn)
{
    return ssd->maptbl[lpn];
}

static inline void set_maptbl_ent(struct ssd* ssd, uint64_t lpn, struct ppa* ppa)
{
    ftl_assert(lpn < ssd->sp.tt_pgs);
    ssd->maptbl[lpn] = *ppa;
}

static uint64_t ppa2pgidx(struct ssd* ssd, struct ppa* ppa)
{
    struct ssdparams* spp = &ssd->sp;
    uint64_t pgidx;

    pgidx = ppa->g.ch * spp->pgs_per_ch + \
        ppa->g.lun * spp->pgs_per_lun + \
        ppa->g.pl * spp->pgs_per_pl + \
        ppa->g.blk * spp->pgs_per_blk + \
        ppa->g.pg;

    ftl_assert(pgidx < spp->tt_pgs);

    return pgidx;
}

static inline uint64_t get_rmap_ent(struct ssd* ssd, struct ppa* ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    return ssd->rmap[pgidx];
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct ssd* ssd, uint64_t lpn, struct ppa* ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    ssd->rmap[pgidx] = lpn;
}

static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static inline pqueue_pri_t victim_line_get_pri(void* a)
{
    return ((StripeInfo*)a)->vpc;
}

static inline void victim_line_set_pri(void* a, pqueue_pri_t pri)
{
    ((StripeInfo*)a)->vpc = pri;
}

static inline size_t victim_line_get_pos(void* a)
{
    return ((StripeInfo*)a)->victimPos;
}

static inline void victim_line_set_pos(void* a, size_t pos)
{
    ((StripeInfo*)a)->victimPos = pos;
}

static inline pqueue_pri_t ECGetPri(void* a) {
    int lid = stripe2line[((StripeInfo*)a)->stripeindex];
    return lineinfo[lid].erasecount;
}

static inline void ECSetPri(void* a, pqueue_pri_t pri) {
    int lid = stripe2line[((StripeInfo*)a)->stripeindex];
    lineinfo[lid].erasecount = pri;
}

static inline int MinCmpPri(pqueue_pri_t next, pqueue_pri_t curr) {
    return (next > curr);
}

static inline size_t WrittenMinGetPos(void* a) {
    return ((StripeInfo*)a)->writtenMinPos;
}

static inline void WrittenMinSetPos(void* a, size_t pos) {
    ((StripeInfo*)a)->writtenMinPos = pos;
}

static void ssd_init_lines(struct ssd* ssd)
{
    struct ssdparams* spp = &ssd->sp;
    struct line_mgmt* lm = &ssd->lm;
    struct line* line;

    lm->tt_lines = spp->tt_lines;
    ftl_assert(lm->tt_lines == spp->tt_lines);
    lm->lines = g_malloc0(sizeof(struct line) * lm->tt_lines);
    lm->free_line_cnt = 0;
    for (int i = 0; i < lm->tt_lines; i++) {
        line = &lm->lines[i];
        line->id = i;
        line->ipc = 0;
        line->vpc = 0;
        line->pos = 0;
        lm->free_line_cnt++;
    }

    ftl_assert(lm->free_line_cnt == lm->tt_lines);
    lm->victim_line_cnt = 0;
    lm->full_line_cnt = 0;

    writtenMinPQ = pqueue_init(totalStripes, MinCmpPri, ECGetPri, ECSetPri, WrittenMinGetPos, WrittenMinSetPos);
    lineinfo = g_malloc0(sizeof(LineInfo) * spp->tt_lines);
    for (int i = 0;i < spp->tt_lines;i += 1) {
        lineinfo[i].ln = &lm->lines[i];
        lineinfo[i].erasecount = 0;
        lineinfo[i].writtenMinPos = 0;
    }

    QTAILQ_INIT(&freeStripeList);
    victimStripePQ = pqueue_init(totalStripes, victim_line_cmp_pri,
        victim_line_get_pri, victim_line_set_pri,
        victim_line_get_pos, victim_line_set_pos);
    QTAILQ_INIT(&fullStripeList);
    double init = UPER(0);
    stripeinfo = g_malloc0(sizeof(StripeInfo) * totalStripes);
    for (int i = 0;i < totalStripes;i += 1) {
        stripeinfo[i].ipc = stripeinfo[i].vpc = 0;
        stripeinfo[i].stripeindex = i * spp->rain_stripe_size;
        stripeinfo[i].upersum = init * spp->rain_stripe_size;
        stripeinfo[i].victimPos = stripeinfo[i].writtenMinPos = 0;
        QTAILQ_INSERT_TAIL(&freeStripeList, &stripeinfo[i], entry);
    }

    line2stripe = g_malloc0(sizeof(int) * spp->tt_lines);
    stripe2line = g_malloc0(sizeof(int) * spp->tt_lines);
    lineuper = g_malloc0(sizeof(double) * spp->tt_lines);
    for (int i = 0;i < spp->tt_lines;i += 1) {
        line2stripe[i] = stripe2line[i] = i;
        lineuper[i] = init;
    }
}

static void ssd_init_write_pointer(struct ssd* ssd)
{
    struct write_pointer* wpp = &ssd->wp;
    struct line_mgmt* lm = &ssd->lm;
    struct line* curline = NULL;

    StripeInfo* st = QTAILQ_FIRST(&freeStripeList);
    QTAILQ_REMOVE(&freeStripeList, st, entry);
    lm->free_line_cnt -= ssd->sp.rain_stripe_size;
    // get line from mapping
    int lid = stripe2line[st->stripeindex];
    curline = &lm->lines[lid];

    /* wpp->curline is always our next-to-write super-block */
    wpp->curline = curline;
    wpp->ch = 0;
    wpp->lun = 0;
    wpp->pg = 0;
    wpp->blk = curline->id;
    wpp->pl = 0;
}

static inline void check_addr(int a, int max)
{
    ftl_assert(a >= 0 && a < max);
}

static struct ppa get_new_page(struct ssd* ssd)
{
    struct write_pointer* wpp = &ssd->wp;
    struct ppa ppa;
    ppa.ppa = 0;
    ppa.g.ch = wpp->ch;
    ppa.g.lun = wpp->lun;
    ppa.g.pg = wpp->pg;
    ppa.g.blk = wpp->blk;
    ppa.g.pl = wpp->pl;
    ftl_assert(ppa.g.pl == 0);

    return ppa;
}

static void ssd_advance_write_pointer(struct ssd* ssd);

static void mark_page_valid(struct ssd* ssd, struct ppa* ppa);

static uint64_t ssd_advance_status(struct ssd* ssd, struct ppa* ppa, struct nand_cmd* ncmd);

// same as gc_write_page except no old_ppa
static uint64_t new_parity_write_page(struct ssd* ssd, struct ppa* new_ppa, uint64_t lpn)
{
    /* update maptbl */
    // set_maptbl_ent(ssd, lpn, &new_ppa);
    /* update rmap */
    set_rmap_ent(ssd, lpn, new_ppa);

    mark_page_valid(ssd, new_ppa);

    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_WRITE;
        gcw.stime = 0;
        ssd_advance_status(ssd, new_ppa, &gcw);
    }

    parityPageWrites += 1;

    return 0;
}

static struct line* get_next_free_line(struct ssd* ssd)
{
    struct line_mgmt* lm = &ssd->lm;
    struct line* curline = NULL;

    StripeInfo* st = QTAILQ_FIRST(&freeStripeList);
    if (!st) {
        ftl_err("No free lines left in [%s] !!!!\n", ssd->ssdname);
        return NULL;
    }
    QTAILQ_REMOVE(&freeStripeList, st, entry);
    lm->free_line_cnt -= ssd->sp.rain_stripe_size;
    // get line from mapping
    int lid = stripe2line[st->stripeindex];
    curline = &lm->lines[lid];
    return curline;
}

static void ssd_advance_write_pointer(struct ssd* ssd)
{
    struct ssdparams* spp = &ssd->sp;
    struct write_pointer* wpp = &ssd->wp;
    struct line_mgmt* lm = &ssd->lm;

    ssdPageWrites += 1;

    currStripeOffset += 1;
    // write order of striping across lines
    int lid = wpp->curline->id;
    int newlid = stripe2line[line2stripe[lid] + 1];
    wpp->curline = &lm->lines[newlid];
    wpp->blk = wpp->curline->id;
    if (currStripeOffset >= spp->rain_stripe_size - 1) {
        // write parity at the end of stripe
        struct ppa ppage = get_new_page(ssd);
        new_parity_write_page(ssd, &ppage, PARITYLPN);
        ssdPageWrites += 1;
        // reset stripe offset
        currStripeOffset = 0;
        int stripeindex = line2stripe[wpp->curline->id];
        assert(stripeindex % spp->rain_stripe_size == spp->rain_stripe_size - 1);
        newlid = stripe2line[stripeindex + 1 - spp->rain_stripe_size];
        wpp->curline = &lm->lines[newlid];
        wpp->blk = wpp->curline->id;
        check_addr(wpp->ch, spp->nchs);
        wpp->ch++;
        if (wpp->ch == spp->nchs) {
            wpp->ch = 0;
            check_addr(wpp->lun, spp->luns_per_ch);
            wpp->lun++;
            /* in this case, we should go to next lun */
            if (wpp->lun == spp->luns_per_ch) {
                wpp->lun = 0;
                /* go to next page in the block */
                check_addr(wpp->pg, spp->pgs_per_blk);
                wpp->pg++;
                if (wpp->pg == spp->pgs_per_blk) {
                    wpp->pg = 0;
                    /* move current line to {victim,full} line list */
                    int sid = line2stripe[wpp->curline->id] / spp->rain_stripe_size;
                    StripeInfo* stripe = &stripeinfo[sid];
                    if (stripe->vpc == spp->pgs_per_line * spp->rain_stripe_size) {
                        QTAILQ_INSERT_TAIL(&fullStripeList, stripe, entry);
                        lm->full_line_cnt += spp->rain_stripe_size;
                    } else {
                        pqueue_insert(victimStripePQ, stripe);
                        lm->victim_line_cnt += spp->rain_stripe_size;
                    }
                    pqueue_insert(writtenMinPQ, stripe);
                    /* current line is used up, pick another empty line */
                    check_addr(wpp->blk, spp->blks_per_pl);
                    wpp->curline = NULL;
                    wpp->curline = get_next_free_line(ssd);
                    if (!wpp->curline) {
                        /* TODO */
                        abort();
                    }
                    wpp->blk = wpp->curline->id;
                    check_addr(wpp->blk, spp->blks_per_pl);
                    /* make sure we are starting from page 0 in the super block */
                    ftl_assert(wpp->pg == 0);
                    ftl_assert(wpp->lun == 0);
                    ftl_assert(wpp->ch == 0);
                    /* TODO: assume # of pl_per_lun is 1, fix later */
                    ftl_assert(wpp->pl == 0);
                }
            }
        }
    }
}

static void check_params(struct ssdparams* spp)
{
    /*
     * we are using a general write pointer increment method now, no need to
     * force luns_per_ch and nchs to be power of 2
     */

     //ftl_assert(is_power_of_2(spp->luns_per_ch));
     //ftl_assert(is_power_of_2(spp->nchs));
}

static void ssd_init_params(struct ssdparams* spp, FemuCtrl* n)
{
    spp->secsz = n->bb_params.secsz; // 512
    spp->secs_per_pg = n->bb_params.secs_per_pg; // 8
    spp->pgs_per_blk = n->bb_params.pgs_per_blk; //256
    spp->blks_per_pl = n->bb_params.blks_per_pl; /* 256 16GB */
    spp->pls_per_lun = n->bb_params.pls_per_lun; // 1
    spp->luns_per_ch = n->bb_params.luns_per_ch; // 8
    spp->nchs = n->bb_params.nchs; // 8

    spp->pg_rd_lat = n->bb_params.pg_rd_lat;
    spp->pg_wr_lat = n->bb_params.pg_wr_lat;
    spp->blk_er_lat = n->bb_params.blk_er_lat;
    spp->ch_xfer_lat = n->bb_params.ch_xfer_lat;

    /* calculated values */
    spp->secs_per_blk = spp->secs_per_pg * spp->pgs_per_blk;
    spp->secs_per_pl = spp->secs_per_blk * spp->blks_per_pl;
    spp->secs_per_lun = spp->secs_per_pl * spp->pls_per_lun;
    spp->secs_per_ch = spp->secs_per_lun * spp->luns_per_ch;
    spp->tt_secs = spp->secs_per_ch * spp->nchs;

    spp->pgs_per_pl = spp->pgs_per_blk * spp->blks_per_pl;
    spp->pgs_per_lun = spp->pgs_per_pl * spp->pls_per_lun;
    spp->pgs_per_ch = spp->pgs_per_lun * spp->luns_per_ch;
    spp->tt_pgs = spp->pgs_per_ch * spp->nchs;

    spp->blks_per_lun = spp->blks_per_pl * spp->pls_per_lun;
    spp->blks_per_ch = spp->blks_per_lun * spp->luns_per_ch;
    spp->tt_blks = spp->blks_per_ch * spp->nchs;

    spp->pls_per_ch = spp->pls_per_lun * spp->luns_per_ch;
    spp->tt_pls = spp->pls_per_ch * spp->nchs;

    spp->tt_luns = spp->luns_per_ch * spp->nchs;

    /* line is special, put it at the end */
    spp->blks_per_line = spp->tt_luns;
    spp->tt_lines = spp->blks_per_lun;
    spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_blk;
    spp->secs_per_line = spp->pgs_per_line * spp->secs_per_pg;

    spp->gc_thres_pcent = n->bb_params.gc_thres_pcent / 100.0;
    spp->gc_thres_lines = (int)((1 - spp->gc_thres_pcent) * spp->tt_lines);
    spp->gc_thres_pcent_rain = n->bb_params.gc_thres_pcent_rain / 100.0;
    spp->gc_thres_lines_rain = (int)((1 - spp->gc_thres_pcent_rain) * spp->tt_lines);
    spp->gc_thres_pcent_high = n->bb_params.gc_thres_pcent_high / 100.0;
    spp->gc_thres_lines_high = (int)((1 - spp->gc_thres_pcent_high) * spp->tt_lines);
    spp->enable_gc_delay = true;


    spp->rain_stripe_size = n->rain_stripe_size;
    assert(spp->rain_stripe_size > 1);
    spp->pwl = n->pwl;
    spp->pagesize = n->bb_params.secsz * n->bb_params.secs_per_pg;
    spp->parity_start_lpn = (uint64_t)n->memsz * 1024 * 1024 / spp->pagesize;
    logicalPages = spp->parity_start_lpn;
    RAINlogicalPages = (spp->parity_start_lpn + spp->rain_stripe_size - 2) / (spp->rain_stripe_size - 1) * spp->rain_stripe_size; // round up to multiple of stripe size
    totalStripes = spp->tt_lines / spp->rain_stripe_size;
    targetErrorRate = UPER(CYCLELIMIT) * logicalPages;
    // adjust cycle limit for stripes
    RAINCycleLimit = CYCLELIMIT;
    while (true) {
        double uper = UPER(RAINCycleLimit);
        double stripeErr = uper * uper * (spp->rain_stripe_size - 1) * spp->rain_stripe_size / 2;
        double err = stripeErr / spp->rain_stripe_size * RAINlogicalPages;
        if (err >= targetErrorRate) {
            break;
        }
        RAINCycleLimit += 1;
    }

    check_params(spp);
}

static void ssd_init_nand_page(struct nand_page* pg, struct ssdparams* spp)
{
    pg->nsecs = spp->secs_per_pg;
    pg->sec = g_malloc0(sizeof(nand_sec_status_t) * pg->nsecs);
    for (int i = 0; i < pg->nsecs; i++) {
        pg->sec[i] = SEC_FREE;
    }
    pg->status = PG_FREE;
}

static void ssd_init_nand_blk(struct nand_block* blk, struct ssdparams* spp)
{
    blk->npgs = spp->pgs_per_blk;
    blk->pg = g_malloc0(sizeof(struct nand_page) * blk->npgs);
    for (int i = 0; i < blk->npgs; i++) {
        ssd_init_nand_page(&blk->pg[i], spp);
    }
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt = 0;
    blk->wp = 0;
}

static void ssd_init_nand_plane(struct nand_plane* pl, struct ssdparams* spp)
{
    pl->nblks = spp->blks_per_pl;
    pl->blk = g_malloc0(sizeof(struct nand_block) * pl->nblks);
    for (int i = 0; i < pl->nblks; i++) {
        ssd_init_nand_blk(&pl->blk[i], spp);
    }
}

static void ssd_init_nand_lun(struct nand_lun* lun, struct ssdparams* spp)
{
    lun->npls = spp->pls_per_lun;
    lun->pl = g_malloc0(sizeof(struct nand_plane) * lun->npls);
    for (int i = 0; i < lun->npls; i++) {
        ssd_init_nand_plane(&lun->pl[i], spp);
    }
    lun->next_lun_avail_time = 0;
    lun->busy = false;
}

static void ssd_init_ch(struct ssd_channel* ch, struct ssdparams* spp)
{
    ch->nluns = spp->luns_per_ch;
    ch->lun = g_malloc0(sizeof(struct nand_lun) * ch->nluns);
    for (int i = 0; i < ch->nluns; i++) {
        ssd_init_nand_lun(&ch->lun[i], spp);
    }
    ch->next_ch_avail_time = 0;
    ch->busy = 0;
}

static void ssd_init_maptbl(struct ssd* ssd)
{
    struct ssdparams* spp = &ssd->sp;

    ssd->maptbl = g_malloc0(sizeof(struct ppa) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->maptbl[i].ppa = UNMAPPED_PPA;
    }
}

static void ssd_init_rmap(struct ssd* ssd)
{
    struct ssdparams* spp = &ssd->sp;

    ssd->rmap = g_malloc0(sizeof(uint64_t) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->rmap[i] = INVALID_LPN;
    }
}

void ssd_init(FemuCtrl* n)
{
    struct ssd* ssd = n->ssd;
    struct ssdparams* spp = &ssd->sp;

    ftl_assert(ssd);

    ssd_init_params(spp, n);

    /* initialize ssd internal layout architecture */
    ssd->ch = g_malloc0(sizeof(struct ssd_channel) * spp->nchs);
    for (int i = 0; i < spp->nchs; i++) {
        ssd_init_ch(&ssd->ch[i], spp);
    }

    /* initialize maptbl */
    ssd_init_maptbl(ssd);

    /* initialize rmap */
    ssd_init_rmap(ssd);

    /* initialize all the lines */
    ssd_init_lines(ssd);

    /* initialize write pointer, this is how we allocate new pages for writes */
    ssd_init_write_pointer(ssd);

    if (true) {
        qemu_thread_create(&trace_thread, "trace-Thread", trace, n, QEMU_THREAD_JOINABLE);
    } else {
        qemu_thread_create(&ssd->ftl_thread, "FEMU-FTL-Thread", ftl_thread, n, QEMU_THREAD_JOINABLE);
    }
}

static inline bool valid_ppa(struct ssd* ssd, struct ppa* ppa)
{
    struct ssdparams* spp = &ssd->sp;
    int ch = ppa->g.ch;
    int lun = ppa->g.lun;
    int pl = ppa->g.pl;
    int blk = ppa->g.blk;
    int pg = ppa->g.pg;
    int sec = ppa->g.sec;

    if (ch >= 0 && ch < spp->nchs && lun >= 0 && lun < spp->luns_per_ch && pl >=
        0 && pl < spp->pls_per_lun && blk >= 0 && blk < spp->blks_per_pl && pg
        >= 0 && pg < spp->pgs_per_blk && sec >= 0 && sec < spp->secs_per_pg)
        return true;

    return false;
}

static inline bool valid_lpn(struct ssd* ssd, uint64_t lpn)
{
    return (lpn < ssd->sp.tt_pgs);
}

static inline bool mapped_ppa(struct ppa* ppa)
{
    return !(ppa->ppa == UNMAPPED_PPA);
}

static inline struct ssd_channel* get_ch(struct ssd* ssd, struct ppa* ppa)
{
    return &(ssd->ch[ppa->g.ch]);
}

static inline struct nand_lun* get_lun(struct ssd* ssd, struct ppa* ppa)
{
    struct ssd_channel* ch = get_ch(ssd, ppa);
    return &(ch->lun[ppa->g.lun]);
}

static inline struct nand_plane* get_pl(struct ssd* ssd, struct ppa* ppa)
{
    struct nand_lun* lun = get_lun(ssd, ppa);
    return &(lun->pl[ppa->g.pl]);
}

static inline struct nand_block* get_blk(struct ssd* ssd, struct ppa* ppa)
{
    struct nand_plane* pl = get_pl(ssd, ppa);
    return &(pl->blk[ppa->g.blk]);
}

static inline struct line* get_line(struct ssd* ssd, struct ppa* ppa)
{
    return &(ssd->lm.lines[ppa->g.blk]);
}

static inline struct nand_page* get_pg(struct ssd* ssd, struct ppa* ppa)
{
    struct nand_block* blk = get_blk(ssd, ppa);
    return &(blk->pg[ppa->g.pg]);
}

static uint64_t ssd_advance_status(struct ssd* ssd, struct ppa* ppa, struct nand_cmd* ncmd)
{
    int c = ncmd->cmd;
    uint64_t cmd_stime = (ncmd->stime == 0) ? \
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;
    uint64_t nand_stime;
    struct ssdparams* spp = &ssd->sp;
    struct nand_lun* lun = get_lun(ssd, ppa);
    uint64_t lat = 0;

    switch (c) {
    case NAND_READ:
        /* read: perform NAND cmd first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
            lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;
        lat = lun->next_lun_avail_time - cmd_stime;
        #if 0
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;

        /* read: then data transfer through channel */
        chnl_stime = (ch->next_ch_avail_time < lun->next_lun_avail_time) ? \
            lun->next_lun_avail_time : ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        lat = ch->next_ch_avail_time - cmd_stime;
        #endif
        break;

    case NAND_WRITE:
        /* write: transfer data through channel first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
            lun->next_lun_avail_time;
        if (ncmd->type == USER_IO) {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        } else {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        }
        lat = lun->next_lun_avail_time - cmd_stime;

        #if 0
        chnl_stime = (ch->next_ch_avail_time < cmd_stime) ? cmd_stime : \
            ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        /* write: then do NAND program */
        nand_stime = (lun->next_lun_avail_time < ch->next_ch_avail_time) ? \
            ch->next_ch_avail_time : lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
        #endif
        break;

    case NAND_ERASE:
        /* erase: only need to advance NAND status */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
            lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->blk_er_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
        break;

    default:
        ftl_err("Unsupported NAND command: 0x%x\n", c);
    }

    return lat;
}

/* update SSD status about one page from PG_VALID -> PG_INVALID */
static void mark_page_invalid(struct ssd* ssd, struct ppa* ppa)
{
    struct line_mgmt* lm = &ssd->lm;
    struct ssdparams* spp = &ssd->sp;
    struct nand_block* blk = NULL;
    struct nand_page* pg = NULL;
    struct line* line;

    /* update corresponding page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_VALID);
    pg->status = PG_INVALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
    blk->ipc++;
    ftl_assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
    blk->vpc--;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    bool was_full_line = false;
    int sid = line2stripe[line->id] / spp->rain_stripe_size;
    StripeInfo* stripe = &stripeinfo[sid];
    stripe->ipc += 1;
    if (stripe->vpc == spp->pgs_per_line * spp->rain_stripe_size) {
        assert(stripe->victimPos == 0);
        was_full_line = true;
    }

    if (stripe->victimPos) {
        /* Note that vpc will be updated by this call */
        pqueue_change_priority(victimStripePQ, stripe->vpc - 1, stripe);
    } else {
        stripe->vpc -= 1;
    }

    if (was_full_line) {
        QTAILQ_REMOVE(&fullStripeList, stripe, entry);
        lm->full_line_cnt -= spp->rain_stripe_size;
        pqueue_insert(victimStripePQ, stripe);
        lm->victim_line_cnt += spp->rain_stripe_size;
    }
}

static void mark_page_valid(struct ssd* ssd, struct ppa* ppa)
{
    struct nand_block* blk = NULL;
    struct nand_page* pg = NULL;
    struct line* line;

    /* update page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_FREE);
    pg->status = PG_VALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->vpc >= 0 && blk->vpc < ssd->sp.pgs_per_blk);
    blk->vpc++;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    int sid = line2stripe[line->id] / ssd->sp.rain_stripe_size;
    StripeInfo* stripe = &stripeinfo[sid];
    stripe->vpc += 1;
}

static void mark_block_free(struct ssd* ssd, struct ppa* ppa)
{
    struct ssdparams* spp = &ssd->sp;
    struct nand_block* blk = get_blk(ssd, ppa);
    struct nand_page* pg = NULL;

    for (int i = 0; i < spp->pgs_per_blk; i++) {
        /* reset page status */
        pg = &blk->pg[i];
        ftl_assert(pg->nsecs == spp->secs_per_pg);
        pg->status = PG_FREE;
    }

    /* reset block status */
    ftl_assert(blk->npgs == spp->pgs_per_blk);
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt++;
}

static void gc_read_page(struct ssd* ssd, struct ppa* ppa)
{
    /* advance ssd status, we don't care about how long it takes */
    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcr;
        gcr.type = GC_IO;
        gcr.cmd = NAND_READ;
        gcr.stime = 0;
        ssd_advance_status(ssd, ppa, &gcr);
    }
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_page(struct ssd* ssd, struct ppa* old_ppa)
{
    struct ppa new_ppa;
    struct nand_lun* new_lun;
    uint64_t lpn = get_rmap_ent(ssd, old_ppa);

    ftl_assert(valid_lpn(ssd, lpn));
    new_ppa = get_new_page(ssd);
    /* update maptbl */
    set_maptbl_ent(ssd, lpn, &new_ppa);
    /* update rmap */
    set_rmap_ent(ssd, lpn, &new_ppa);

    mark_page_valid(ssd, &new_ppa);

    /* need to advance the write pointer here */
    ssd_advance_write_pointer(ssd);

    GCPageWrites += 1;

    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_WRITE;
        gcw.stime = 0;
        ssd_advance_status(ssd, &new_ppa, &gcw);
    }

    /* advance per-ch gc_endtime as well */
    #if 0
    new_ch = get_ch(ssd, &new_ppa);
    new_ch->gc_endtime = new_ch->next_ch_avail_time;
    #endif

    new_lun = get_lun(ssd, &new_ppa);
    new_lun->gc_endtime = new_lun->next_lun_avail_time;

    return 0;
}

static StripeInfo* select_victim_stripe(struct ssd* ssd, bool force)
{
    struct line_mgmt* lm = &ssd->lm;
    struct ssdparams* spp = &ssd->sp;
    StripeInfo* stripe = pqueue_peek(victimStripePQ);
    if (!stripe) {
        return NULL;
    }

    if (!force && stripe->ipc < spp->pgs_per_line * spp->rain_stripe_size / 8) {
        return NULL;
    }

    pqueue_pop(victimStripePQ);
    stripe->victimPos = 0;
    lm->victim_line_cnt -= spp->rain_stripe_size;

    pqueue_remove(writtenMinPQ, stripe);
    stripe->writtenMinPos = 0;

    /* victim_line is a danggling node now */
    return stripe;
}

/* here ppa identifies the block we want to clean */
static void clean_one_block(struct ssd* ssd, struct ppa* ppa)
{
    struct ssdparams* spp = &ssd->sp;
    struct nand_page* pg_iter = NULL;
    int cnt = 0;

    for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
        ppa->g.pg = pg;
        pg_iter = get_pg(ssd, ppa);
        /* there shouldn't be any free page in victim blocks */
        ftl_assert(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID && get_rmap_ent(ssd, ppa) != PARITYLPN) {
            gc_read_page(ssd, ppa);
            /* delay the maptbl update until "write" happens */
            gc_write_page(ssd, ppa);
            cnt++;
        }
    }

    ftl_assert(get_blk(ssd, ppa)->vpc == cnt);
}

static void MoveColdData(struct ssd* ssd, StripeInfo* cold, StripeInfo* free) {
    struct ssdparams* spp = &ssd->sp;
    // move all valid and invalid data
    free->ipc = cold->ipc;
    free->vpc = cold->vpc;
    if (cold->vpc == spp->pgs_per_line * spp->rain_stripe_size) {
        assert(cold->victimPos == 0);
        QTAILQ_REMOVE(&fullStripeList, cold, entry);
        QTAILQ_INSERT_TAIL(&fullStripeList, free, entry);
    } else {
        assert(cold->victimPos > 0);
        pqueue_remove(victimStripePQ, cold);
        pqueue_insert(victimStripePQ, free);
        cold->victimPos = 0;
    }
    pqueue_insert(writtenMinPQ, free);
    cold->ipc = 0;
    cold->vpc = 0;

    double newsum = 0.0;
    // move all lines in stripe
    for (int offset = 0;offset < spp->rain_stripe_size;offset += 1) {
        int coldlineid = stripe2line[cold->stripeindex + offset];
        int freelineid = stripe2line[free->stripeindex + offset];
        struct line* coldline = lineinfo[coldlineid].ln;
        struct line* freeline = lineinfo[freelineid].ln;
        struct ppa coldppa, freeppa;
        coldppa.ppa = freeppa.ppa = 0;
        coldppa.g.blk = coldline->id;
        freeppa.g.blk = freeline->id;
        for (int currblk = 0;currblk < spp->blks_per_line;currblk += 1) {
            struct nand_block* coldblk = get_blk(ssd, &coldppa);
            struct nand_block* freeblk = get_blk(ssd, &freeppa);
            freeblk->ipc = coldblk->ipc;
            freeblk->vpc = coldblk->vpc;
            coldblk->ipc = 0;
            coldblk->vpc = 0;
            coldblk->erase_cnt += 1;

            // read and write all pages
            for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
                coldppa.g.pg = pg;
                freeppa.g.pg = pg;
                struct nand_page* pg_iter = &coldblk->pg[pg];
                gc_read_page(ssd, &coldppa);
                if (pg_iter->status == PG_VALID) {
                    uint64_t lpn = get_rmap_ent(ssd, &coldppa);
                    set_maptbl_ent(ssd, lpn, &freeppa);
                    set_rmap_ent(ssd, lpn, &freeppa);
                    freeblk->pg[pg].status = PG_VALID;
                }
                pg_iter->status = PG_FREE;
                if (ssd->sp.enable_gc_delay) {
                    struct nand_cmd gcw;
                    gcw.type = GC_IO;
                    gcw.cmd = NAND_WRITE;
                    gcw.stime = 0;
                    ssd_advance_status(ssd, &freeppa, &gcw);
                }
                struct nand_lun* free_lun = get_lun(ssd, &freeppa);
                free_lun->gc_endtime = free_lun->next_lun_avail_time;
                PWLPageWrites += 1;
                ssdPageWrites += 1;
            }

            // erase the cold line
            if (spp->enable_gc_delay) {
                struct nand_cmd gce;
                gce.type = GC_IO;
                gce.cmd = NAND_ERASE;
                gce.stime = 0;
                ssd_advance_status(ssd, &coldppa, &gce);
            }
            struct nand_lun* lun = get_lun(ssd, &coldppa);
            lun->gc_endtime = lun->next_lun_avail_time;

            coldppa.g.ch += 1;
            if (coldppa.g.ch == spp->nchs) {
                coldppa.g.lun += 1;
                coldppa.g.ch = 0;
            }
            freeppa.g.ch = coldppa.g.ch;
            freeppa.g.lun = coldppa.g.lun;
        }
        lineinfo[coldlineid].erasecount += 1;
        lineuper[coldlineid] = UPER(lineinfo[coldlineid].erasecount);
        newsum += lineuper[coldlineid];
    }
    cold->upersum = newsum;
}

static void mark_stripe_free(struct ssd* ssd, struct ppa* ppa)
{
    struct ssdparams* spp = &ssd->sp;
    struct line_mgmt* lm = &ssd->lm;
    struct line* line = get_line(ssd, ppa);
    int sid = line2stripe[line->id] / ssd->sp.rain_stripe_size;
    StripeInfo* stripe = &stripeinfo[sid];
    stripe->ipc = 0;
    stripe->vpc = 0;

    if (spp->pwl > 0) {
        // do wear leveling according to threshold
        double avgEC = (double)gcCount / totalStripes;
        double threshold = avgEC * (100 - spp->pwl) / 100 + (double)RAINCycleLimit * (spp->pwl) / 100;
        LineInfo* curr = &lineinfo[line->id];
        if (curr->erasecount > threshold) {
            StripeInfo* minwritten = pqueue_pop(writtenMinPQ);
            minwritten->writtenMinPos = 0;
            MoveColdData(ssd, minwritten, stripe);
            QTAILQ_INSERT_TAIL(&freeStripeList, minwritten, entry);
        } else {
            QTAILQ_INSERT_TAIL(&freeStripeList, stripe, entry);
        }
    } else {
        QTAILQ_INSERT_TAIL(&freeStripeList, stripe, entry);
    }
    lm->free_line_cnt += spp->rain_stripe_size;
}

static int do_gc(struct ssd* ssd, bool force)
{
    // struct line* victim_line = NULL;
    struct ssdparams* spp = &ssd->sp;
    struct nand_lun* lunp;

    StripeInfo* victim_stripe = select_victim_stripe(ssd, force);
    if (!victim_stripe) {
        return -1;
    }

    struct ppa ppa;
    ppa.ppa = 0;
    double newsum = 0.0;
    // GC all lines in stripe
    for (int offset = 0;offset < spp->rain_stripe_size;offset += 1) {
        int lineid = stripe2line[victim_stripe->stripeindex + offset];
        ppa.ppa = 0;
        ppa.g.blk = lineid;

        /* copy back valid data */
        for (int currblk = 0;currblk < spp->blks_per_line;currblk += 1) {
            lunp = get_lun(ssd, &ppa);
            clean_one_block(ssd, &ppa);
            mark_block_free(ssd, &ppa);

            if (spp->enable_gc_delay) {
                struct nand_cmd gce;
                gce.type = GC_IO;
                gce.cmd = NAND_ERASE;
                gce.stime = 0;
                ssd_advance_status(ssd, &ppa, &gce);
            }

            lunp->gc_endtime = lunp->next_lun_avail_time;

            if (currblk < spp->blks_per_line - 1) {
                ppa.g.ch += 1;
                if (ppa.g.ch == spp->nchs) {
                    ppa.g.lun += 1;
                    ppa.g.ch = 0;
                }
            }
        }

        /* update line status */
        lineinfo[lineid].erasecount += 1;
        lineuper[lineid] = UPER(lineinfo[lineid].erasecount);
        newsum += lineuper[lineid];
    }
    victim_stripe->upersum = newsum;
    gcCount += 1;
    mark_stripe_free(ssd, &ppa);

    if (gcCount % (totalStripes * 10) == 0) {
        int cycles = ssdPageWrites / spp->tt_pgs;
        fprintf(outfp, "\ncycles %d cyclelimit %d gccount %d\n", cycles, RAINCycleLimit, gcCount);
        fprintf(outfp, "hostpages %lu ssdpages %lu gcpages %lu paritypages %lu wlpages %lu WAF %e\n", hostPageWrites, ssdPageWrites, GCPageWrites, parityPageWrites, PWLPageWrites, (double)ssdPageWrites / hostPageWrites);
        dumpBlocks(ssd);
        fflush(outfp);
    }

    return 0;
}

static uint64_t ssd_read(struct ssd* ssd, NvmeRequest* req)
{
    struct ssdparams* spp = &ssd->sp;
    uint64_t lba = req->slba;
    int nsecs = req->nlb;
    struct ppa ppa;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + nsecs - 1) / spp->secs_per_pg;
    uint64_t lpn;
    uint64_t sublat, maxlat = 0;

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }

    /* normal IO read path */
    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        ppa = get_maptbl_ent(ssd, lpn);
        if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
            continue;
        }

        struct nand_cmd srd;
        srd.type = USER_IO;
        srd.cmd = NAND_READ;
        srd.stime = req->stime;
        sublat = ssd_advance_status(ssd, &ppa, &srd);
        maxlat = (sublat > maxlat) ? sublat : maxlat;
    }

    return maxlat;
}

static uint64_t ssd_write(struct ssd* ssd, NvmeRequest* req)
{
    uint64_t lba = req->slba;
    struct ssdparams* spp = &ssd->sp;
    int len = req->nlb;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg;
    struct ppa ppa;
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0;
    int r;

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }

    while (should_gc_high(ssd)) {
        /* perform GC here until !should_gc(ssd) */
        r = do_gc(ssd, true);
        if (r == -1)
            break;
    }

    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        ppa = get_maptbl_ent(ssd, lpn);
        if (mapped_ppa(&ppa)) {
            /* update old page information first */
            mark_page_invalid(ssd, &ppa);
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
        }

        /* new write */
        ppa = get_new_page(ssd);
        /* update maptbl */
        set_maptbl_ent(ssd, lpn, &ppa);
        /* update rmap */
        set_rmap_ent(ssd, lpn, &ppa);

        mark_page_valid(ssd, &ppa);

        /* need to advance the write pointer here */
        ssd_advance_write_pointer(ssd);

        struct nand_cmd swr;
        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = req->stime;
        /* get latency statistics */
        curlat = ssd_advance_status(ssd, &ppa, &swr);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }
    hostPageWrites += end_lpn - start_lpn + 1;

    return maxlat;
}

static void ResetState(struct ssd* ssd) {
    struct ssdparams* spp = &ssd->sp;
    for (int i = 0; i < spp->nchs; i++) {
        struct ssd_channel* ch = &ssd->ch[i];
        for (int j = 0; j < ch->nluns; j++) {
            struct nand_lun* lun = &ch->lun[j];
            for (int k = 0; k < lun->npls; k++) {
                struct nand_plane* pl = &lun->pl[k];
                for (int l = 0; l < pl->nblks; l++) {
                    struct nand_block* blk = &pl->blk[l];
                    for (int m = 0; m < blk->npgs; m++) {
                        struct nand_page* pg = &blk->pg[m];
                        for (int n = 0; n < pg->nsecs; n++) {
                            pg->sec[n] = SEC_FREE;
                        }
                        pg->status = PG_FREE;
                    }
                    blk->ipc = 0;
                    blk->vpc = 0;
                    blk->erase_cnt = 0;
                    blk->wp = 0;
                }
            }
            lun->next_lun_avail_time = 0;
            lun->busy = false;
        }
        ch->next_ch_avail_time = 0;
        ch->busy = 0;
    }

    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->maptbl[i].ppa = UNMAPPED_PPA;
        ssd->rmap[i] = INVALID_LPN;
    }

    struct line_mgmt* lm = &ssd->lm;
    struct line* line;
    // QTAILQ_INIT(&lm->free_line_list);
    // lm->victim_line_pq->size = 1;
    // QTAILQ_INIT(&lm->full_line_list);

    lm->free_line_cnt = lm->tt_lines;
    lm->victim_line_cnt = 0;
    lm->full_line_cnt = 0;
    for (int i = 0; i < lm->tt_lines; i++) {
        line = &lm->lines[i];
        line->id = i;
        line->ipc = 0;
        line->vpc = 0;
        line->pos = 0;
        /* initialize all the lines as free lines */
        // QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
    }

    writtenMinPQ->size = 1;
    for (int i = 0;i < spp->tt_lines;i += 1) {
        lineinfo[i].ln = &lm->lines[i];
        lineinfo[i].erasecount = 0;
        lineinfo[i].writtenMinPos = 0;
    }

    QTAILQ_INIT(&freeStripeList);
    victimStripePQ->size = 1;
    QTAILQ_INIT(&fullStripeList);
    double init = UPER(0);
    for (int i = 0;i < totalStripes;i += 1) {
        stripeinfo[i].ipc = stripeinfo[i].vpc = 0;
        stripeinfo[i].stripeindex = i * spp->rain_stripe_size;
        stripeinfo[i].upersum = init * spp->rain_stripe_size;
        stripeinfo[i].victimPos = stripeinfo[i].writtenMinPos = 0;
        QTAILQ_INSERT_TAIL(&freeStripeList, &stripeinfo[i], entry);
    }
    for (int i = 0;i < spp->tt_lines;i += 1) {
        line2stripe[i] = stripe2line[i] = i;
        lineuper[i] = init;
    }

    ssd_init_write_pointer(ssd);

    gcCount = currStripeOffset = 0;
    hostPageWrites = ssdPageWrites = GCPageWrites = parityPageWrites = PWLPageWrites = 0;
    currErrorRate = 0;
}

static void DiskTrace(FemuCtrl* n) {
    struct ssd* ssd = n->ssd;
    uint64_t offset = 0, len = 0;
    NvmeRequest rq;
    rq.stime = 0;

    char buf[256];
    sprintf(buf, "/home/ubuntu/share/alibabatrace/alibaba_block_traces_2020/sizeGB%d/diskids%d", n->tracediskGB, n->tracefile);
    printf("tracefile %s\n", buf);

    int diskid = 0;
    FILE* fp = fopen(buf, "r");
    while (fscanf(fp, "%d", &diskid) != EOF) {
        sprintf(buf, "/home/ubuntu/share/alibabatrace/alibaba_block_traces_2020/sizeGB%d/reforge/%d+1/disk%dprefillPPNRAINPWL%d", n->tracediskGB, n->rain_stripe_size - 1, diskid, n->pwl);
        outfp = fopen(buf, "w");
        printf("outfile %s\n", buf);

        sprintf(buf, "/home/ubuntu/share/alibabatrace/alibaba_block_traces_2020/sizeGB%d/readprefill/disk%d", n->tracediskGB, diskid);
        FILE* fpin = fopen(buf, "r");
        while (fscanf(fpin, "%lu %lu", &offset, &len) != EOF) {
            rq.slba = offset / ssd->sp.secsz;
            rq.nlb = len / ssd->sp.secsz;

            ssd_write(ssd, &rq);
            if (should_gc(ssd)) {
                do_gc(ssd, false);
            }
        }
        fclose(fpin);

        sprintf(buf, "/home/ubuntu/share/alibabatrace/alibaba_block_traces_2020/sizeGB%d/write/disk%d", n->tracediskGB, diskid);
        while (currErrorRate < targetErrorRate) {
            fpin = fopen(buf, "r");
            while (fscanf(fpin, "%lu %lu", &offset, &len) != EOF) {
                rq.slba = offset / ssd->sp.secsz;
                rq.nlb = len / ssd->sp.secsz;

                ssd_write(ssd, &rq);
                if (should_gc(ssd)) {
                    do_gc(ssd, false);
                }

                if (currErrorRate >= targetErrorRate) {
                    break;
                }
            }
            fclose(fpin);
        }

        fflush(outfp);
        fclose(outfp);
        ResetState(ssd);
    }
    fclose(fp);
}

static void SynthTrace(FemuCtrl* n) {
    struct ssd* ssd = n->ssd;
    uint64_t offset = 0, len = 0;
    NvmeRequest rq;
    rq.stime = 0;

    #define ROUNDS 1
    const double rs[ROUNDS] = { 0.5 }, hs[ROUNDS] = { 0.5 };
    const int cycles[ROUNDS] = { 8 }, footprints[ROUNDS] = { 10 };

    for (int i = 0;i < ROUNDS;i += 1) {
        double r = rs[i], h = hs[i];
        int traceCycle = cycles[i], footprint = footprints[i];
        char buf[256];
        sprintf(buf, "/home/ubuntu/share/alibabatrace/alibaba_block_traces_2020/synthetic/r%gh%gfootprint%dsize20GB%dcycle%d+1/diskids%d", r, h, footprint, traceCycle, n->rain_stripe_size - 1, n->tracefile);
        printf("tracefile %s\n", buf);

        int full = 0;
        FILE* fp = fopen(buf, "r");
        while (fscanf(fp, "%d", &full) != EOF) {
            sprintf(buf, "/home/ubuntu/share/alibabatrace/alibaba_block_traces_2020/synthetic/r%gh%gfootprint%dsize20GB%dcycle%d+1/PPNRAINPWL%dfull%d", r, h, footprint, traceCycle, n->rain_stripe_size - 1, n->pwl, full);
            outfp = fopen(buf, "w");
            printf("outfile %s\n", buf);

            int prefillmb = 20 * 1024 * full / 100;
            for (int mb = 0;mb < prefillmb;mb += 1) {
                offset = (uint64_t)mb * 1024 * 1024;
                len = 1024 * 1024;
                rq.slba = offset / ssd->sp.secsz;
                rq.nlb = len / ssd->sp.secsz;

                ssd_write(ssd, &rq);
                if (should_gc(ssd)) {
                    do_gc(ssd, false);
                }
            }
            printf("\nprefill %d MB\n", prefillmb);

            while (currErrorRate < targetErrorRate) {
                sprintf(buf, "/home/ubuntu/share/alibabatrace/alibaba_block_traces_2020/synthetic/r%gh%gfootprint%dsize20GB%dcycletrace", r, h, footprint, traceCycle);
                FILE* fpin = fopen(buf, "r");
                while (fscanf(fpin, "%lu %lu", &offset, &len) != EOF) {
                    rq.slba = offset / ssd->sp.secsz;
                    rq.nlb = len / ssd->sp.secsz;

                    ssd_write(ssd, &rq);
                    if (should_gc(ssd)) {
                        do_gc(ssd, false);
                    }

                    if (currErrorRate >= targetErrorRate) {
                        break;
                    }
                }
                fclose(fpin);
            }
            fflush(outfp);
            fclose(outfp);
            ResetState(ssd);
        }
        fclose(fp);
    }
}

static void* trace(void* arg) {
    FemuCtrl* n = (FemuCtrl*)arg;
    if (n->tracediskGB > 0) {
        DiskTrace(n);
    } else {
        SynthTrace(n);
    }
    abort();

    return NULL;
}

static void* ftl_thread(void* arg)
{
    FemuCtrl* n = (FemuCtrl*)arg;
    struct ssd* ssd = n->ssd;
    NvmeRequest* req = NULL;
    uint64_t lat = 0;
    int rc;
    int i;

    while (!*(ssd->dataplane_started_ptr)) {
        usleep(100000);
    }

    /* FIXME: not safe, to handle ->to_ftl and ->to_poller gracefully */
    ssd->to_ftl = n->to_ftl;
    ssd->to_poller = n->to_poller;

    qemu_thread_create(&trace_thread, "trace-Thread", trace, n, QEMU_THREAD_JOINABLE);

    while (1) {
        for (i = 1; i <= n->nr_pollers; i++) {
            if (!ssd->to_ftl[i] || !femu_ring_count(ssd->to_ftl[i]))
                continue;

            rc = femu_ring_dequeue(ssd->to_ftl[i], (void*)&req, 1);
            if (rc != 1) {
                printf("FEMU: FTL to_ftl dequeue failed\n");
            }

            ftl_assert(req);
            switch (req->cmd.opcode) {
            case NVME_CMD_WRITE:
                lat = ssd_write(ssd, req);
                break;
            case NVME_CMD_READ:
                lat = ssd_read(ssd, req);
                break;
            case NVME_CMD_DSM:
                lat = 0;
                break;
            default:
                //ftl_err("FTL received unkown request type, ERROR\n");
                ;
            }

            req->reqlat = lat;
            req->expire_time += lat;

            rc = femu_ring_enqueue(ssd->to_poller[i], (void*)&req, 1);
            if (rc != 1) {
                ftl_err("FTL to_poller enqueue failed\n");
            }

            /* clean one line if needed (in the background) */
            if (should_gc(ssd)) {
                do_gc(ssd, false);
            }
        }
    }

    return NULL;
}
