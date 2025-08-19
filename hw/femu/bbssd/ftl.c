#include <math.h>
#include <stdlib.h>
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
    size_t victimPos, writtenMinPos, freeMinPos, freeMaxPos; // for priority queue
} StripeInfo;

typedef struct LINESTATUS {
    int lineid, erasecount;
    bool isfree, isparity;
} LineStatus;

typedef struct UPERINDEX {
    double uper;
    int lineid;
    bool chosen;
} UPERIndex;

// should reset
static uint64_t hostPageWrites = 0, ssdPageWrites = 0, GCPageWrites = 0, parityPageWrites = 0, PWLPageWrites = 0;
static int gcCount = 0, currStripeOffset = 0, swapFreeCount = 0, clearFullCount = 0, markoutLines = 0, reforge = 0; // reforge 0: before, 1: reforging, 2: finished, prevent starting reforge again when gc in reforge
static double currErrorRate = 0, UPERsum = 0;
static FILE* outfp;
static LineInfo* lineinfo;
static StripeInfo* stripeinfo;
pqueue_t* writtenMinECPQ, * victimStripePQ, * victimStripePQsuperl, * freeMinUPERPQ, * freeMaxUPERPQ; // priority queue for PWL and SUPERL
QTAILQ_HEAD(freeStripeList, StripeInfo) freeStripeList;
QTAILQ_HEAD(fullStripeList, StripeInfo) fullStripeList;
static int* stripe2line = NULL, * line2stripe = NULL; // mapping between line id and stripe, remains the same in this baseline case
static double* lineuper;

// remains unchanged after init
static const uint64_t PARITYLPN = INVALID_LPN - 1;
int logicalPages = 0, RAINlogicalPages = 0, totalStripes = 0, RAINCycleLimit = 0, markoutLimit = 0;
static double targetErrorRate = 0;

// buffer for reforge and SUPERL
static UPERIndex* UPERBuffer;
static LineStatus* LineECBuffer;

// RBER model parameters
static const double K = 2.05;
static const double ALPHA = 3.9e-10;
static const double EPSILON = 1.48e-3;
static const int ECC = 128;
static const int CYCLELIMIT = 900;

static void reforgeSSD(struct ssd* ssd);

// for sorting lines according to erase count
static inline int CmpEC(const void* a, const void* b) {
    return ((LineStatus*)a)->erasecount - ((LineStatus*)b)->erasecount;
}

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
    struct line_mgmt* lm = &ssd->lm;
    double p = 0, maxupersum = 0, minupersum = 0;
    int maxupersumstripe = 0, minupersumstripe = 0;
    int lpages = logicalPages;
    if (reforge == 2) {
        lpages = RAINlogicalPages;
        // parity stripes UPER
        for (int s = 0; s < totalStripes; s++) {
            int stripeindex = s * spp->rain_stripe_size;
            double minus = 0, sum = 0;
            for (int o = 0;o < spp->rain_stripe_size;o += 1) {
                int ln = stripe2line[stripeindex + o];
                sum += lineuper[ln];
                minus += lineuper[ln] * lineuper[ln];
            }
            assert(sum == stripeinfo[s].upersum);
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
        if (spp->superl > 0) {
            StripeInfo* maxfree = pqueue_peek(freeMaxUPERPQ);
            StripeInfo* minfree = pqueue_peek(freeMinUPERPQ);
            fprintf(outfp, "maxfreesum %d %e minfreesum %d %e\n", maxfree->stripeindex / spp->rain_stripe_size, maxfree->upersum, minfree->stripeindex / spp->rain_stripe_size, minfree->upersum);
        }
        p = p / spp->tt_lines * lpages; // scale to logical size
    } else {
        // ECC lines UPER
        int validLines = 0;
        maxupersum = -1;
        minupersum = -1;
        for (int s = 0; s < spp->tt_lines; s++) {
            if (lm->lines[s].vpc != -1) {
                // not markout line
                double stripeuper = stripeinfo[s].upersum;
                assert(stripeuper == UPER(lineinfo[s].erasecount));
                if (maxupersum < 0 || stripeuper > maxupersum) {
                    maxupersum = stripeuper;
                    maxupersumstripe = s;
                }
                if (minupersum < 0 || stripeuper < minupersum) {
                    minupersum = stripeuper;
                    minupersumstripe = s;
                }
                p += stripeuper;
                validLines += 1;
            }
        }
        assert(validLines + markoutLines == spp->tt_lines);
        p = p / validLines * lpages; // scale to logical size
    }
    fprintf(outfp, "maxupersum %d %e minupersum %d %e\n", maxupersumstripe, maxupersum, minupersumstripe, minupersum);
    return p;
}

void dumpBlocks(struct ssd* ssd) {
    struct ssdparams* spp = &ssd->sp;
    currErrorRate = DevicePFail(ssd);
    fprintf(outfp, "devicep %e targetp %e\n", currErrorRate, targetErrorRate);
    if (reforge != 1 && currErrorRate >= targetErrorRate) {
        // lines grouped by stripes
        fprintf(outfp, "\nlineerasecount\n");
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

        // construct parity stripes if not reforged
        if (reforge == 0) {
            reforgeSSD(ssd);
        }
    }
}

static void* ftl_thread(void* arg);

static inline bool should_gc(struct ssd* ssd)
{
    if (reforge == 2) {
        return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines_rain);
    }
    return (ssd->lm.free_line_cnt + markoutLines <= ssd->sp.gc_thres_lines);
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
    assert(lpn < ssd->sp.tt_pgs);
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

    assert(pgidx < spp->tt_pgs);

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

static inline int victim_line_cmp_pri_superl(pqueue_pri_t next, pqueue_pri_t curr)
{
    double n = *((double*)(&next)), c = *((double*)(&curr));
    return (n > c);
}

static inline pqueue_pri_t victim_line_get_pri_superl(void* a)
{
    double pri = ((StripeInfo*)a)->upersum * ((StripeInfo*)a)->vpc;
    return *((pqueue_pri_t*)(&pri));
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

static inline int MinECCmpPri(pqueue_pri_t next, pqueue_pri_t curr) {
    return (next > curr);
}

static inline pqueue_pri_t UPERGetPri(void* a) {
    return *((pqueue_pri_t*)(&((StripeInfo*)a)->upersum));
}

static inline void UPERSetPri(void* a, pqueue_pri_t pri) {
    ((StripeInfo*)a)->upersum = *((double*)(&pri));
}

static inline int MinUPERCmpPri(pqueue_pri_t next, pqueue_pri_t curr) {
    double n = *((double*)(&next)), c = *((double*)(&curr));
    return (n > c);
}

static inline int MaxUPERCmpPri(pqueue_pri_t next, pqueue_pri_t curr) {
    double n = *((double*)(&next)), c = *((double*)(&curr));
    return (n < c);
}

static inline size_t WrittenMinGetPos(void* a) {
    return ((StripeInfo*)a)->writtenMinPos;
}

static inline void WrittenMinSetPos(void* a, size_t pos) {
    ((StripeInfo*)a)->writtenMinPos = pos;
}

static inline size_t FreeMinGetPos(void* a) {
    return ((StripeInfo*)a)->freeMinPos;
}

static inline void FreeMinSetPos(void* a, size_t pos) {
    ((StripeInfo*)a)->freeMinPos = pos;
}

static inline size_t FreeMaxGetPos(void* a) {
    return ((StripeInfo*)a)->freeMaxPos;
}

static inline void FreeMaxSetPos(void* a, size_t pos) {
    ((StripeInfo*)a)->freeMaxPos = pos;
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

    writtenMinECPQ = pqueue_init(spp->tt_lines, MinECCmpPri, ECGetPri, ECSetPri, WrittenMinGetPos, WrittenMinSetPos);
    freeMinUPERPQ = pqueue_init(totalStripes, MinUPERCmpPri, UPERGetPri, UPERSetPri, FreeMinGetPos, FreeMinSetPos);
    freeMaxUPERPQ = pqueue_init(totalStripes, MaxUPERCmpPri, UPERGetPri, UPERSetPri, FreeMaxGetPos, FreeMaxSetPos);
    lineinfo = g_malloc0(sizeof(LineInfo) * spp->tt_lines);
    for (int i = 0;i < spp->tt_lines;i += 1) {
        lineinfo[i].ln = &lm->lines[i];
        lineinfo[i].erasecount = 0;
        lineinfo[i].writtenMinPos = 0;
    }

    // initial stripe is same as line
    QTAILQ_INIT(&freeStripeList);
    victimStripePQ = pqueue_init(spp->tt_lines, victim_line_cmp_pri, victim_line_get_pri, victim_line_set_pri, victim_line_get_pos, victim_line_set_pos);
    victimStripePQsuperl = pqueue_init(spp->tt_lines, victim_line_cmp_pri_superl, victim_line_get_pri_superl, victim_line_set_pri, victim_line_get_pos, victim_line_set_pos);
    QTAILQ_INIT(&fullStripeList);
    double init = UPER(0);
    stripeinfo = g_malloc0(sizeof(StripeInfo) * spp->tt_lines);
    for (int i = 0;i < spp->tt_lines;i += 1) {
        stripeinfo[i].ipc = stripeinfo[i].vpc = 0;
        stripeinfo[i].stripeindex = i;
        stripeinfo[i].upersum = init;
        stripeinfo[i].victimPos = stripeinfo[i].writtenMinPos = stripeinfo[i].freeMaxPos = stripeinfo[i].freeMinPos = 0;
        QTAILQ_INSERT_TAIL(&freeStripeList, &stripeinfo[i], entry);
    }
    UPERsum = init * spp->tt_lines;

    line2stripe = g_malloc0(sizeof(int) * spp->tt_lines);
    stripe2line = g_malloc0(sizeof(int) * spp->tt_lines);
    lineuper = g_malloc0(sizeof(double) * spp->tt_lines);
    for (int i = 0;i < spp->tt_lines;i += 1) {
        line2stripe[i] = stripe2line[i] = i;
        lineuper[i] = init;
    }

    UPERBuffer = g_malloc0(sizeof(UPERIndex) * spp->rain_stripe_size * 2);
    LineECBuffer = g_malloc0(sizeof(LineStatus) * spp->tt_lines);
}

static void ssd_init_write_pointer(struct ssd* ssd)
{
    struct write_pointer* wpp = &ssd->wp;
    struct line_mgmt* lm = &ssd->lm;
    struct line* curline = NULL;

    StripeInfo* st = QTAILQ_FIRST(&freeStripeList);
    QTAILQ_REMOVE(&freeStripeList, st, entry);
    lm->free_line_cnt -= 1;
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
    struct ssdparams* spp = &ssd->sp;
    struct line_mgmt* lm = &ssd->lm;
    struct line* curline = NULL;

    StripeInfo* st;
    int linecnt = 1;
    if (reforge == 2) {
        linecnt = spp->rain_stripe_size;
    }
    if (reforge == 2 && spp->superl > 0) {
        st = pqueue_peek(freeMinUPERPQ);
        if (!st) {
            ftl_err("No free lines left in [%s] !!!!\n", ssd->ssdname);
            return NULL;
        }
        pqueue_pop(freeMinUPERPQ);
        pqueue_remove(freeMaxUPERPQ, st);
    } else {
        st = QTAILQ_FIRST(&freeStripeList);
        if (!st) {
            ftl_err("No free lines left in [%s] !!!!\n", ssd->ssdname);
            return NULL;
        }
        QTAILQ_REMOVE(&freeStripeList, st, entry);
    }
    lm->free_line_cnt -= linecnt;

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

    if (reforge == 2) {
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
        } else {
            return;
        }
    }

    // advance to next stripe if reforged, next page if not
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
                if (reforge == 2) {
                    int sid = line2stripe[wpp->curline->id] / spp->rain_stripe_size;
                    StripeInfo* stripe = &stripeinfo[sid];
                    if (spp->superl > 0) {
                        pqueue_insert(victimStripePQsuperl, stripe);
                    } else {
                        pqueue_insert(victimStripePQ, stripe);
                    }
                    lm->victim_line_cnt += spp->rain_stripe_size;
                } else {
                    int sid = line2stripe[wpp->curline->id];
                    StripeInfo* stripe = &stripeinfo[sid];
                    pqueue_insert(writtenMinECPQ, stripe);
                    if (stripe->vpc == spp->pgs_per_line) {
                        QTAILQ_INSERT_TAIL(&fullStripeList, stripe, entry);
                        lm->full_line_cnt += 1;
                    } else {
                        pqueue_insert(victimStripePQ, stripe);
                        lm->victim_line_cnt += 1;
                    }
                }

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
    spp->superl = n->superl;
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
    int logicalLines = logicalPages / spp->pgs_per_line;
    markoutLimit = totalStripes;
    assert(logicalLines + markoutLimit + spp->gc_thres_lines_high < spp->tt_lines);

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

    qemu_thread_create(&ssd->ftl_thread, "FEMU-FTL-Thread", ftl_thread, n,
        QEMU_THREAD_JOINABLE);
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
    assert(pg->status == PG_VALID);
    pg->status = PG_INVALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
    blk->ipc++;
    assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
    blk->vpc--;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    if (reforge == 2) {
        int sid = line2stripe[line->id] / spp->rain_stripe_size;
        StripeInfo* stripe = &stripeinfo[sid];
        stripe->ipc += 1;
        if (stripe->victimPos > 0) {
            if (spp->superl > 0) {
                pqueue_change_priority(victimStripePQsuperl, stripe->vpc - 1, stripe);
            } else {
                pqueue_change_priority(victimStripePQ, stripe->vpc - 1, stripe);
            }
        } else {
            stripe->vpc -= 1;
        }
    } else {
        bool was_full_line = false;
        int sid = line2stripe[line->id];
        StripeInfo* stripe = &stripeinfo[sid];
        stripe->ipc += 1;
        if (stripe->vpc == spp->pgs_per_line) {
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
            lm->full_line_cnt -= 1;
            pqueue_insert(victimStripePQ, stripe);
            lm->victim_line_cnt += 1;
        }
    }
}

static void mark_page_valid(struct ssd* ssd, struct ppa* ppa)
{
    struct nand_block* blk = NULL;
    struct nand_page* pg = NULL;
    struct line* line;

    /* update page status */
    pg = get_pg(ssd, ppa);
    assert(pg->status == PG_FREE);
    pg->status = PG_VALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    assert(blk->vpc >= 0 && blk->vpc < ssd->sp.pgs_per_blk);
    blk->vpc++;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    int sid = line->id;
    if (reforge == 2) {
        sid = line2stripe[line->id] / ssd->sp.rain_stripe_size;
    }
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

    assert(valid_lpn(ssd, lpn));
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

    pqueue_t* victimpq = victimStripePQ;
    int linecnt = 1;
    if (reforge == 2) {
        linecnt = spp->rain_stripe_size;
        if (spp->superl > 0) {
            victimpq = victimStripePQsuperl;
        }
    }
    StripeInfo* stripe = pqueue_peek(victimpq);
    if (!stripe) {
        return NULL;
    }
    if (!force && stripe->ipc < spp->pgs_per_line * linecnt / 8) {
        return NULL;
    }

    pqueue_pop(victimpq);
    stripe->victimPos = 0;
    lm->victim_line_cnt -= linecnt;

    if (reforge < 2) {
        pqueue_remove(writtenMinECPQ, stripe);
    }

    /* victim_line is a danggling node now */
    return stripe;
}

/* here ppa identifies the block we want to clean */
static void clean_one_block(struct ssd* ssd, struct ppa* ppa)
{
    struct ssdparams* spp = &ssd->sp;
    struct nand_page* pg_iter = NULL;

    for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
        ppa->g.pg = pg;
        pg_iter = get_pg(ssd, ppa);
        /* there shouldn't be any free page in victim blocks */
        // ftl_assert(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) {
            if (get_rmap_ent(ssd, ppa) != PARITYLPN) {
                gc_read_page(ssd, ppa);
                /* delay the maptbl update until "write" happens */
                gc_write_page(ssd, ppa);
            }
        }
    }
}

static void InsSort(UPERIndex* arr, int size) {
    int i, j;
    UPERIndex tmp;
    for (i = 1; i < size; i++) {
        if (arr[i].uper < arr[i - 1].uper) {
            tmp = arr[i];
            for (j = i - 1; j >= 0 && arr[j].uper > tmp.uper; j--) {
                arr[j + 1] = arr[j];
            }
            arr[j + 1] = tmp;
        }
    }
}

static void SwapFreeStripe(struct ssd* ssd, StripeInfo* a, StripeInfo* b) {
    struct ssdparams* spp = &ssd->sp;
    double sum = a->upersum + b->upersum;
    double target = sum / 2;
    // copy uper of lines in both stripes to buffer
    for (int i = 0;i < spp->rain_stripe_size;i += 1) {
        int lid = stripe2line[a->stripeindex + i];
        UPERBuffer[i].lineid = lid;
        UPERBuffer[i].uper = lineuper[lid];
        UPERBuffer[i].chosen = false;
    }
    for (int i = 0;i < spp->rain_stripe_size;i += 1) {
        int lid = stripe2line[b->stripeindex + i];
        UPERBuffer[i + spp->rain_stripe_size].lineid = lid;
        UPERBuffer[i + spp->rain_stripe_size].uper = lineuper[lid];
        UPERBuffer[i + spp->rain_stripe_size].chosen = false;
    }
    InsSort(UPERBuffer, spp->rain_stripe_size * 2);

    int head = 0, tail = spp->rain_stripe_size * 2 - 1;
    double stripesum = 0;
    int j = 0;
    // select uper in descending order until max but not exceeding target
    for (;j < spp->rain_stripe_size - 1;j++) {
        UPERBuffer[tail].chosen = true;
        stripesum += UPERBuffer[tail].uper;
        tail--;
        if (stripesum + UPERBuffer[tail].uper > target) {
            j++;
            break;
        }
    }
    // select smallest upers until chosen (stripesize - 1) lines
    for (;j < spp->rain_stripe_size - 1;j++) {
        UPERBuffer[head].chosen = true;
        stripesum += UPERBuffer[head].uper;
        head++;
    }
    // decide the last uper which adds up closest to target
    double delta = 1 + sum, lasttarget = target - stripesum;
    int k = head;
    for (;k <= tail;k++) {
        double d = fabs(UPERBuffer[k].uper - lasttarget);
        if (d >= delta) {
            // uper is ascending, so the previous uper is closest
            break;
        }
        delta = d;
    }
    // select the previous one
    k -= 1;
    UPERBuffer[k].chosen = true;

    head = tail = 0;
    a->upersum = b->upersum = 0;
    for (int i = 0;i < spp->rain_stripe_size * 2;i += 1) {
        if (UPERBuffer[i].chosen) {
            stripe2line[a->stripeindex + head] = UPERBuffer[i].lineid;
            line2stripe[UPERBuffer[i].lineid] = a->stripeindex + head;
            lineuper[UPERBuffer[i].lineid] = UPERBuffer[i].uper;
            a->upersum += UPERBuffer[i].uper;
            head += 1;
        } else {
            stripe2line[b->stripeindex + tail] = UPERBuffer[i].lineid;
            line2stripe[UPERBuffer[i].lineid] = b->stripeindex + tail;
            lineuper[UPERBuffer[i].lineid] = UPERBuffer[i].uper;
            b->upersum += UPERBuffer[i].uper;
            tail += 1;
        }
    }
    assert(head == tail && head == spp->rain_stripe_size);
}

static void MoveColdData(struct ssd* ssd, StripeInfo* ColdStripe, StripeInfo* FreeStripe) {
    struct ssdparams* spp = &ssd->sp;
    int linecnt = 1;
    // move all valid and invalid data
    FreeStripe->ipc = ColdStripe->ipc;
    FreeStripe->vpc = ColdStripe->vpc;
    if (reforge == 2) {
        linecnt = spp->rain_stripe_size;
        if (spp->superl > 0) {
            pqueue_remove(victimStripePQsuperl, ColdStripe);
            pqueue_insert(victimStripePQsuperl, FreeStripe);
            ColdStripe->victimPos = 0;
        } else {
            pqueue_remove(victimStripePQ, ColdStripe);
            pqueue_insert(victimStripePQ, FreeStripe);
            ColdStripe->victimPos = 0;
        }
    } else {
        if (ColdStripe->vpc == spp->pgs_per_line) {
            assert(ColdStripe->victimPos == 0);
            QTAILQ_REMOVE(&fullStripeList, ColdStripe, entry);
            QTAILQ_INSERT_TAIL(&fullStripeList, FreeStripe, entry);
        } else {
            assert(ColdStripe->victimPos > 0);
            pqueue_remove(victimStripePQ, ColdStripe);
            pqueue_insert(victimStripePQ, FreeStripe);
            ColdStripe->victimPos = 0;
        }
        pqueue_insert(writtenMinECPQ, FreeStripe);
    }
    ColdStripe->ipc = 0;
    ColdStripe->vpc = 0;

    double newsum = 0.0;
    // move all lines in stripe
    for (int offset = 0;offset < linecnt;offset += 1) {
        int coldlineid = stripe2line[ColdStripe->stripeindex + offset];
        int freelineid = stripe2line[FreeStripe->stripeindex + offset];
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
    UPERsum += (newsum - ColdStripe->upersum);
    ColdStripe->upersum = newsum;
}

static void mark_stripe_free(struct ssd* ssd, struct ppa* ppa)
{
    struct ssdparams* spp = &ssd->sp;
    struct line_mgmt* lm = &ssd->lm;
    struct line* line = get_line(ssd, ppa);

    int linecnt = 1;
    int sid = line2stripe[line->id];
    if (reforge == 2) {
        linecnt = spp->rain_stripe_size;
        sid /= ssd->sp.rain_stripe_size;
    }
    StripeInfo* stripe = &stripeinfo[sid];
    stripe->ipc = 0;
    stripe->vpc = 0;

    if (reforge == 2) {
        if (spp->superl > 0) {
            StripeInfo* curr = &stripeinfo[sid];
            pqueue_insert(freeMinUPERPQ, curr);
            pqueue_insert(freeMaxUPERPQ, curr);

            StripeInfo* minfree = pqueue_peek(freeMinUPERPQ);
            StripeInfo* maxfree = pqueue_peek(freeMaxUPERPQ);
            if (maxfree != minfree) {
                // more than 1 free stripe, swap largest with smallest
                pqueue_pop(freeMaxUPERPQ);
                pqueue_pop(freeMinUPERPQ);
                pqueue_remove(freeMaxUPERPQ, minfree);
                pqueue_remove(freeMinUPERPQ, maxfree);

                SwapFreeStripe(ssd, minfree, maxfree);
                swapFreeCount += 1;

                pqueue_insert(freeMinUPERPQ, minfree);
                pqueue_insert(freeMinUPERPQ, maxfree);
                pqueue_insert(freeMaxUPERPQ, minfree);
                pqueue_insert(freeMaxUPERPQ, maxfree);
            }

            // maxfree = pqueue_peek(freeMaxUPERPQ);
            // minfree = pqueue_peek(freeMinUPERPQ);
            // StripeInfo* minwritten = pqueue_peek(writtenMinUPERPQ);
            // if (minwritten != NULL && minfree->upersum > minwritten->upersum * 100) {

            //     fprintf(outfp, "maxfreesum %e minfreesum %e minwrittensum %e\n", maxfree->upersum, minfree->upersum, minwritten->upersum);
            //     fflush(outfp);

            //     // minwritten is likely to be cold, move data
            //     if (true) {
            //         pqueue_pop(writtenMinUPERPQ);
            //         pqueue_pop(freeMaxUPERPQ);
            //         pqueue_remove(freeMinUPERPQ, maxfree);
            //         MoveColdData(ssd, minwritten, maxfree);
            //     } else {
            //         // swap stripe may not be effective, use minfree instead of maxfree
            //         pqueue_pop(writtenMinUPERPQ);
            //         pqueue_pop(freeMinUPERPQ);
            //         pqueue_remove(freeMaxUPERPQ, minfree);
            //         MoveColdData(ssd, minwritten, minfree);
            //     }
            //     clearFullCount += 1;

            //     // now minwritten is free, swap with largest
            //     maxfree = pqueue_peek(freeMaxUPERPQ);
            //     if (maxfree != NULL) {
            //         pqueue_pop(freeMaxUPERPQ);
            //         pqueue_remove(freeMinUPERPQ, maxfree);

            //         SwapFreeStripe(ssd, minwritten, maxfree);
            //         swapFreeCount += 1;

            //         pqueue_insert(freeMinUPERPQ, maxfree);
            //         pqueue_insert(freeMaxUPERPQ, maxfree);
            //     }
            //     pqueue_insert(freeMinUPERPQ, minwritten);
            //     pqueue_insert(freeMaxUPERPQ, minwritten);
            // }
        } else {
            QTAILQ_INSERT_TAIL(&freeStripeList, stripe, entry);
        }
    } else {
        if (markoutLines < markoutLimit && lineinfo[line->id].erasecount >= CYCLELIMIT) {
            line->ipc = 0;
            line->vpc = -1; // markout this line
            markoutLines += 1;
            return;
        } else {
            if (spp->pwl > 0) {
                // do wear leveling according to threshold
                double avgEC = (double)gcCount / spp->tt_lines;
                double threshold = avgEC * (100 - spp->pwl) / 100 + (double)CYCLELIMIT * (spp->pwl) / 100;
                LineInfo* curr = &lineinfo[line->id];
                if (curr->erasecount > threshold) {
                    StripeInfo* minwritten = pqueue_pop(writtenMinECPQ);
                    minwritten->writtenMinPos = 0;
                    MoveColdData(ssd, minwritten, stripe);
                    QTAILQ_INSERT_TAIL(&freeStripeList, minwritten, entry);
                } else {
                    QTAILQ_INSERT_TAIL(&freeStripeList, stripe, entry);
                }
            } else {
                /* move this line to free line list */
                QTAILQ_INSERT_TAIL(&freeStripeList, stripe, entry);
            }
        }
    }
    lm->free_line_cnt += linecnt;
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

    int linecnt = 1, eclimit = CYCLELIMIT;
    if (reforge == 2) {
        linecnt = spp->rain_stripe_size;
        eclimit = RAINCycleLimit;
    }
    struct ppa ppa;
    ppa.ppa = 0;
    double newsum = 0.0;
    // GC all lines in stripe
    for (int offset = 0;offset < linecnt;offset += 1) {
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
    UPERsum += (newsum - victim_stripe->upersum);
    victim_stripe->upersum = newsum;
    gcCount += 1;
    mark_stripe_free(ssd, &ppa);

    if (gcCount % (spp->tt_lines / linecnt * 10) == 0) {
        int cycles = ssdPageWrites / spp->tt_pgs;
        fprintf(outfp, "\ncycles %d cyclelimit %d gccount %d swapfree %d clearfull %d markoutlines %d\n", cycles, eclimit, gcCount, swapFreeCount, clearFullCount, markoutLines);
        fprintf(outfp, "hostpages %lu ssdpages %lu gcpages %lu paritypages %lu wlpages %lu WAF %e\n", hostPageWrites, ssdPageWrites, GCPageWrites, parityPageWrites, PWLPageWrites, (double)ssdPageWrites / hostPageWrites);
        fprintf(outfp, "upersum %e target %e\n", UPERsum, UPERsum / (spp->tt_lines / linecnt));
        dumpBlocks(ssd);
        fflush(outfp);
    }

    return 0;
}

static void StaticStripes(struct ssd* ssd) {
    struct ssdparams* spp = &ssd->sp;

    // find (totalStripes + spp->rain_stripe_size - 1) free lines
    int parityCount = 0;
    for (int i = 0;i < spp->tt_lines;i += 1) {
        if (LineECBuffer[i].isfree) {
            LineECBuffer[i].isparity = true;
            parityCount += 1;
            if (parityCount >= totalStripes + spp->rain_stripe_size - 1) {
                break;
            }
        }
    }
    assert(parityCount == totalStripes + spp->rain_stripe_size - 1);

    // 1 free line + fulllines
    int fullhead = 0, freehead = 0;
    int curr = 0;
    while (fullhead < spp->tt_lines && LineECBuffer[fullhead].isparity) {
        fullhead += 1;
    }
    while (freehead < spp->tt_lines && !LineECBuffer[freehead].isparity) {
        freehead += 1;
    }
    assert((!LineECBuffer[fullhead].isparity) && LineECBuffer[freehead].isparity);
    // select the first free stripe
    for (int j = 0;j < spp->rain_stripe_size;j += 1) {
        stripe2line[curr + j] = LineECBuffer[freehead].lineid;
        line2stripe[LineECBuffer[freehead].lineid] = curr + j;

        freehead += 1;
        while (freehead < spp->tt_lines && !LineECBuffer[freehead].isparity) {
            freehead += 1;
        }
    }
    curr += spp->rain_stripe_size;
    // select 1 free line each step
    for (int i = 0;i < totalStripes - 1;i += 1) {
        for (int j = 0;j < spp->rain_stripe_size - 1;j += 1) {
            stripe2line[curr + j] = LineECBuffer[fullhead].lineid;
            line2stripe[LineECBuffer[fullhead].lineid] = curr + j;

            fullhead += 1;
            while (fullhead < spp->tt_lines && LineECBuffer[fullhead].isparity) {
                fullhead += 1;
            }
        }
        stripe2line[curr + spp->rain_stripe_size - 1] = LineECBuffer[freehead].lineid;
        line2stripe[LineECBuffer[freehead].lineid] = curr + spp->rain_stripe_size - 1;

        freehead += 1;
        while (freehead < spp->tt_lines && !LineECBuffer[freehead].isparity) {
            freehead += 1;
        }

        curr += spp->rain_stripe_size;
    }
    assert(freehead == spp->tt_lines && fullhead == spp->tt_lines);

    // verify stripes
    for (int i = 0;i < spp->tt_lines;i += 1) {
        LineECBuffer[i].isparity = false;
    }
    curr = 0;
    while (curr < spp->tt_lines) {
        int freelines = 0;
        for (int o = 0;o < spp->rain_stripe_size;o += 1) {
            int lineid = stripe2line[curr + o];
            assert(line2stripe[lineid] == curr + o);
            if (stripeinfo[lineid].vpc == 0 && stripeinfo[lineid].ipc == 0) {
                freelines += 1;
            }
            // make sure each line is in only 1 stripe
            assert(!LineECBuffer[lineid].isparity);
            LineECBuffer[lineid].isparity = true;
        }
        assert(freelines >= 1);
        if (curr == 0) {
            // free stripe
            assert(freelines == spp->rain_stripe_size);
        }
        curr += spp->rain_stripe_size;
    }
    for (int i = 0;i < spp->tt_lines;i += 1) {
        assert(LineECBuffer[i].isparity);
    }
}

static void BuildStripes(struct ssd* ssd) {
    struct ssdparams* spp = &ssd->sp;

    int parityCount = 0, freetail = -1;
    for (int i = spp->tt_lines - 1;i >= 0 && parityCount < totalStripes;i -= 1) {
        if (LineECBuffer[i].isfree) {
            if (freetail < 0) {
                freetail = i;
            }
            // select free lines from tail for parity
            LineECBuffer[i].isparity = true;
            parityCount += 1;
        }
    }
    assert(parityCount == totalStripes);
    // select (stripesize - 1) lines for the free stripe
    for (int i = 0;i < spp->tt_lines && parityCount < totalStripes + spp->rain_stripe_size - 1;i += 1) {
        if (LineECBuffer[i].isfree) {
            assert(!LineECBuffer[i].isparity);
            LineECBuffer[i].isparity = true;
            parityCount += 1;
        }
    }
    assert(parityCount == totalStripes + spp->rain_stripe_size - 1);

    // 1 old + young, select 1 parity each step
    int fullhead = 0, freehead = 0;
    int curr = 0, tail = spp->tt_lines - 1;
    while (fullhead < spp->tt_lines && LineECBuffer[fullhead].isparity) {
        fullhead += 1;
    }
    while (freehead < spp->tt_lines && !LineECBuffer[freehead].isparity) {
        freehead += 1;
    }
    assert((!LineECBuffer[fullhead].isparity) && LineECBuffer[freehead].isparity);
    // select the first free stripe
    stripe2line[curr + spp->rain_stripe_size - 1] = LineECBuffer[freetail].lineid;
    line2stripe[LineECBuffer[freetail].lineid] = curr + spp->rain_stripe_size - 1;
    for (int j = 0;j < spp->rain_stripe_size - 1;j += 1) {
        stripe2line[curr + j] = LineECBuffer[freehead].lineid;
        line2stripe[LineECBuffer[freehead].lineid] = curr + j;

        freehead += 1;
        while (freehead < spp->tt_lines && !LineECBuffer[freehead].isparity) {
            freehead += 1;
        }
    }
    curr += spp->rain_stripe_size;
    // select 1 free line each step
    for (int i = 0;i < totalStripes - 1;i += 1) {
        if (tail == freetail) {
            tail -= 1;
        }
        if (LineECBuffer[tail].isparity) {
            // 1 old parity + (n - 1) young full
            stripe2line[curr + spp->rain_stripe_size - 1] = LineECBuffer[tail].lineid;
            line2stripe[LineECBuffer[tail].lineid] = curr + spp->rain_stripe_size - 1;
            for (int j = 0;j < spp->rain_stripe_size - 1;j += 1) {
                stripe2line[curr + j] = LineECBuffer[fullhead].lineid;
                line2stripe[LineECBuffer[fullhead].lineid] = curr + j;

                fullhead += 1;
                while (fullhead < spp->tt_lines && LineECBuffer[fullhead].isparity) {
                    fullhead += 1;
                }
            }
        } else {
            // 1 old full + 1 young parity + (n - 2) young full
            for (int j = 0;j < spp->rain_stripe_size - 2;j += 1) {
                stripe2line[curr + j] = LineECBuffer[fullhead].lineid;
                line2stripe[LineECBuffer[fullhead].lineid] = curr + j;

                fullhead += 1;
                while (fullhead < spp->tt_lines && LineECBuffer[fullhead].isparity) {
                    fullhead += 1;
                }
            }
            stripe2line[curr + spp->rain_stripe_size - 2] = LineECBuffer[tail].lineid;
            line2stripe[LineECBuffer[tail].lineid] = curr + spp->rain_stripe_size - 2;

            stripe2line[curr + spp->rain_stripe_size - 1] = LineECBuffer[freehead].lineid;
            line2stripe[LineECBuffer[freehead].lineid] = curr + spp->rain_stripe_size - 1;

            freehead += 1;
            while (freehead < spp->tt_lines && !LineECBuffer[freehead].isparity) {
                freehead += 1;
            }
        }
        curr += spp->rain_stripe_size;
        tail -= 1;
    }
    
    // verify stripes
    for (int i = 0;i < spp->tt_lines;i += 1) {
        LineECBuffer[i].isparity = false;
    }
    curr = 0;
    while (curr < spp->tt_lines) {
        int freelines = 0;
        for (int o = 0;o < spp->rain_stripe_size;o += 1) {
            int lineid = stripe2line[curr + o];
            assert(line2stripe[lineid] == curr + o);
            if (stripeinfo[lineid].vpc == 0 && stripeinfo[lineid].ipc == 0) {
                freelines += 1;
            }
            // make sure each line is in only 1 stripe
            assert(!LineECBuffer[lineid].isparity);
            LineECBuffer[lineid].isparity = true;
        }
        assert(freelines >= 1);
        if (curr == 0) {
            // free stripe
            assert(freelines == spp->rain_stripe_size);
        }
        curr += spp->rain_stripe_size;
    }
    for (int i = 0;i < spp->tt_lines;i += 1) {
        assert(LineECBuffer[i].isparity);
    }
}

static void reforgeSSD(struct ssd* ssd) {
    struct ssdparams* spp = &ssd->sp;
    struct line_mgmt* lm = &ssd->lm;
    struct write_pointer* wpp = &ssd->wp;

    reforge = 1;
    // do gc until enough free lines for parity
    uint64_t gcPages = GCPageWrites;
    int gccnt = gcCount;
    fprintf(outfp, "\nstartreforgefreelines %d victimlines %d fulllines %d\n", lm->free_line_cnt, lm->victim_line_cnt, lm->full_line_cnt);
    while (lm->free_line_cnt + markoutLines < totalStripes + spp->rain_stripe_size - 1) {
        do_gc(ssd, true);
        gcCount += 1;
    }
    fprintf(outfp, "startreforgegccount %d reforgegcpages %lu freelines %d\n", gcCount - gccnt, GCPageWrites - gcPages, lm->free_line_cnt);
    // find free lines for parity
    int freelines = 0;
    for (int i = 0;i < spp->tt_lines;i += 1) {
        LineECBuffer[i].lineid = i;
        LineECBuffer[i].erasecount = lineinfo[i].erasecount;
        if (stripeinfo[i].vpc == 0 && stripeinfo[i].ipc == 0) {
            LineECBuffer[i].isfree = true;
            freelines += 1;
        } else {
            LineECBuffer[i].isfree = false;
        }
        LineECBuffer[i].isparity = false;
    }
    assert(freelines >= totalStripes + spp->rain_stripe_size - 1);
    if (spp->superl < 0) {
        // static stripe layout
        StaticStripes(ssd);
    } else {
        // sort lines according to erase count and build stripes
        qsort(LineECBuffer, spp->tt_lines, sizeof(LineStatus), CmpEC);
        BuildStripes(ssd);
    }

    // write parity for valid pages
    int curr = 0;
    while (curr < spp->tt_lines) {
        struct ppa currppa;
        currppa.ppa = 0;
        for (int currblk = 0;currblk < spp->blks_per_line;currblk += 1) {
            for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
                currppa.g.pg = pg;
                int validcnt = 0;
                // read written pages and write parity if this page level stripe has valid page
                for (int o = 0;o < spp->rain_stripe_size;o += 1) {
                    int lineid = stripe2line[curr + o];
                    currppa.g.blk = lineid;
                    struct nand_page* currpage = get_pg(ssd, &currppa);
                    if (curr == 0) {
                        assert(stripeinfo[lineid].vpc == 0 && stripeinfo[lineid].ipc == 0);
                        assert(currpage->status == PG_FREE);
                    }
                    if (o < spp->rain_stripe_size - 1) {
                        if (currpage->status == PG_VALID) {
                            assert(get_rmap_ent(ssd, &currppa) != PARITYLPN);
                            gc_read_page(ssd, &currppa);
                            validcnt += 1;
                        } else if (currpage->status == PG_INVALID) {
                            gc_read_page(ssd, &currppa);
                        }
                    } else {
                        assert(currpage->status == PG_FREE);
                        if (validcnt > 0) {
                            new_parity_write_page(ssd, &currppa, PARITYLPN);
                            ssdPageWrites += 1;
                        }
                    }
                }
            }
            currppa.g.ch += 1;
            if (currppa.g.ch == spp->nchs) {
                currppa.g.lun += 1;
                currppa.g.ch = 0;
            }
        }
        curr += spp->rain_stripe_size;
    }
    fprintf(outfp, "reforgewriteparitypages %ld\n", parityPageWrites);

    // update global info
    for (int i = 0;i < spp->tt_lines;i += 1) {
        lm->lines[i].ipc = stripeinfo[i].ipc;
        lm->lines[i].vpc = stripeinfo[i].vpc;
    }
    double maxupersum = 0, minupersum = 0;
    UPERsum = currErrorRate = 0;
    lm->free_line_cnt = lm->victim_line_cnt = lm->full_line_cnt = 0;
    QTAILQ_INIT(&freeStripeList);
    victimStripePQ->size = 1;
    QTAILQ_INIT(&fullStripeList);
    for (int i = 0;i < totalStripes;i += 1) {
        // update stripeinfo
        int stripeindex = i * spp->rain_stripe_size;
        stripeinfo[i].stripeindex = stripeindex;
        stripeinfo[i].ipc = stripeinfo[i].vpc = 0;
        stripeinfo[i].upersum = 0;
        stripeinfo[i].victimPos = stripeinfo[i].writtenMinPos = 0;
        double minus = 0;
        for (int o = 0;o < spp->rain_stripe_size;o += 1) {
            int lineid = stripe2line[stripeindex + o];
            if (i == 0) {
                assert(lm->lines[lineid].vpc == 0 && lm->lines[lineid].ipc == 0);
            }
            stripeinfo[i].ipc += lm->lines[lineid].ipc;
            stripeinfo[i].vpc += lm->lines[lineid].vpc;
            assert(lineuper[lineid] == UPER(lineinfo[lineid].erasecount));
            stripeinfo[i].upersum += lineuper[lineid];
            minus += lineuper[lineid] * lineuper[lineid];
        }

        if (i > 0) {
            if (i == 1 || stripeinfo[i].upersum < minupersum) {
                minupersum = stripeinfo[i].upersum;
            }
            if (i == 1 || stripeinfo[i].upersum > maxupersum) {
                maxupersum = stripeinfo[i].upersum;
            }
        } else {
            assert(stripeinfo[i].vpc == 0 && stripeinfo[i].ipc == 0);
        }
        UPERsum += stripeinfo[i].upersum;
        currErrorRate += (stripeinfo[i].upersum * stripeinfo[i].upersum - minus) / 2;

        if (stripeinfo[i].vpc == 0 && stripeinfo[i].ipc == 0) {
            // free stripe
            lm->free_line_cnt += spp->rain_stripe_size;
            if (spp->superl > 0) {
                pqueue_insert(freeMaxUPERPQ, &stripeinfo[i]);
                pqueue_insert(freeMinUPERPQ, &stripeinfo[i]);
            } else {
                QTAILQ_INSERT_TAIL(&freeStripeList, &stripeinfo[i], entry);
            }
        } else {
            // victim stripe
            lm->victim_line_cnt += spp->rain_stripe_size;
            if (spp->superl > 0) {
                pqueue_insert(victimStripePQsuperl, &stripeinfo[i]);
            } else {
                pqueue_insert(victimStripePQ, &stripeinfo[i]);
            }
        }
    }
    currErrorRate = currErrorRate / spp->tt_lines * RAINlogicalPages;
    fprintf(outfp, "freeupersum %e maxupersum %e minupersum %e\n", stripeinfo[0].upersum, maxupersum, minupersum);
    fprintf(outfp, "currErrorRate %e upersum %e\n", currErrorRate, UPERsum);
    fprintf(outfp, "endreforgefreelines %d victimlines %d fulllines %d\n", lm->free_line_cnt, lm->victim_line_cnt, lm->full_line_cnt);
    fflush(outfp);

    // update write pointer
    reforge = 2;
    wpp->curline = get_next_free_line(ssd);
    wpp->ch = 0;
    wpp->lun = 0;
    wpp->pl = 0;
    wpp->blk = wpp->curline->id;
    wpp->pg = 0;
    
    // do gc until free ratio is normal
    gccnt = gcCount;
    gcPages = GCPageWrites;
    while (should_gc_high(ssd)) {
        do_gc(ssd, true);
        gcCount += 1;
    }
    fprintf(outfp, "endreforgegccount %d reforgegcpages %lu freelines %d\n", gcCount - gccnt, GCPageWrites - gcPages, lm->free_line_cnt);
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

    writtenMinECPQ->size = 1;
    freeMinUPERPQ->size = 1;
    freeMaxUPERPQ->size = 1;
    for (int i = 0;i < spp->tt_lines;i += 1) {
        lineinfo[i].ln = &lm->lines[i];
        lineinfo[i].erasecount = 0;
        lineinfo[i].writtenMinPos = 0;
    }

    QTAILQ_INIT(&freeStripeList);
    victimStripePQ->size = 1;
    victimStripePQsuperl->size = 1;
    QTAILQ_INIT(&fullStripeList);
    double init = UPER(0);
    for (int i = 0;i < spp->tt_lines;i += 1) {
        stripeinfo[i].ipc = stripeinfo[i].vpc = 0;
        stripeinfo[i].stripeindex = i;
        stripeinfo[i].upersum = init;
        stripeinfo[i].victimPos = stripeinfo[i].writtenMinPos = stripeinfo[i].freeMaxPos = stripeinfo[i].freeMinPos = 0;
        QTAILQ_INSERT_TAIL(&freeStripeList, &stripeinfo[i], entry);
    }
    for (int i = 0;i < spp->tt_lines;i += 1) {
        line2stripe[i] = stripe2line[i] = i;
        lineuper[i] = init;
    }
    UPERsum = init * spp->tt_lines;

    ssd_init_write_pointer(ssd);

    gcCount = currStripeOffset = swapFreeCount = clearFullCount = markoutLines = 0;
    hostPageWrites = ssdPageWrites = GCPageWrites = parityPageWrites = PWLPageWrites = 0;
    currErrorRate = 0;
    reforge = 0;
}

static QemuThread trace_thread;

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
        sprintf(buf, "/home/ubuntu/share/alibabatrace/alibaba_block_traces_2020/sizeGB%d/reforge/disk%dprefillReforgePWL%dSUPERL%d", n->tracediskGB, diskid, n->pwl, n->superl);
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

    int traceCycle = 4;
    char buf[256];
    sprintf(buf, "/home/ubuntu/share/alibabatrace/alibaba_block_traces_2020/synthetic/r0.9h0.1footprint100size20GB%dcycle%d+1/diskids%d", traceCycle, n->rain_stripe_size - 1, n->tracefile);
    printf("tracefile %s\n", buf);

    int full = 0;
    FILE* fp = fopen(buf, "r");
    while (fscanf(fp, "%d", &full) != EOF) {
        sprintf(buf, "/home/ubuntu/share/alibabatrace/alibaba_block_traces_2020/synthetic/r0.9h0.1footprint100size20GB%dcycle%d+1/ReforgePWL%dSUPERL%dfull%d", traceCycle, n->rain_stripe_size - 1, n->pwl, n->superl, full);
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
            sprintf(buf, "/home/ubuntu/share/alibabatrace/alibaba_block_traces_2020/synthetic/r0.9h0.1footprint100size20GB%dcycletrace", traceCycle);
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

static void* trace(void* arg) {
    sleep(30);
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
