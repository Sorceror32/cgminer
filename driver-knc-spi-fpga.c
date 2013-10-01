/* cgminer driver for KnCminer Jupiter */

#include <stdlib.h>
#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <linux/types.h>
#include <linux/spi/spidev.h>

#define DO_SELFTEST 1
#define	ENABLE_SPI_HW
/* Benchmark = ignore actual SPI responses, use fake ones */
/* #define	ENABLE_BENCHMARK */

#include "logging.h"
#include "miner.h"

#define MAX_SPIS		1
#define	MAX_BYTES_IN_SPI_XSFER	4096
/* /dev/spidevB.C, where B = bus, C = chipselect */
#define SPI_DEVICE_TEMPLATE	"/dev/spidev%d.%d"
#define SPI_MODE		(SPI_CPHA | SPI_CPOL | SPI_CS_HIGH)
#define SPI_BITS_PER_WORD	32
#define SPI_MAX_SPEED		3000000
#define SPI_DELAY_USECS		0
/* Max number of ASICs permitted on one SPI device */
#define MAX_ASICS		6

/* How many hardware errors in a row before disabling the core */
#define HW_ERR_LIMIT		10

#define MAX_ACTIVE_WORKS	(192 * 2 * 6 * 2)

#define WORK_MIDSTATE_WORDS	8
#define WORK_DATA_WORDS		3

#define WORK_STALE_US		60000000

struct spidev_context {
	int fd;
	uint32_t speed;
	uint16_t delay;
	uint8_t mode;
	uint8_t bits;
};

struct spi_request {
#define	CMD_NOP		0
#define	CMD_GET_VERSION	1
#define	CMD_SUBMIT_WORK	2
#define	CMD_FLUSH_QUEUE	3

#define	WORK_ID_MASK	0x7FFF

#if (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
	uint32_t cmd		:4;
	uint32_t rsvd		:1; /* set to zero */
	uint32_t queue_id	:12;
	uint32_t work_id	:15;
#else
	uint32_t work_id	:15;
	uint32_t queue_id	:12;
	uint32_t rsvd		:1; /* set to zero */
	uint32_t cmd		:4;
#endif
	uint32_t midstate[WORK_MIDSTATE_WORDS];
	uint32_t data[WORK_DATA_WORDS];
};

struct spi_response {
#define	RESPONSE_TYPE_NOP		0
#define	RESPONSE_TYPE_NONCE_FOUND	1
#define	RESPONSE_TYPE_WORK_DONE		2
#if (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
	uint32_t type		:2;
	uint32_t asic		:3;
	uint32_t queue_id	:12;
	uint32_t work_id	:15;
#else
	uint32_t work_id	:15;
	uint32_t queue_id	:12;
	uint32_t asic		:3;
	uint32_t type		:2;
#endif
	uint32_t nonce;
	uint32_t core;
};

#define MAX_REQUESTS_IN_BATCH	( MAX_BYTES_IN_SPI_XSFER /	\
				  sizeof(struct spi_request)	\
				)

static struct spi_request spi_txbuf[MAX_REQUESTS_IN_BATCH];

#define MAX_RESPONSES_IN_BATCH	( (sizeof(spi_txbuf) - 12) /	\
				   sizeof(struct spi_response)	\
				)

struct spi_rx_t {
#if (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
	uint32_t rsvd_1			:31;
	uint32_t response_queue_full	:1;
#else
	uint32_t response_queue_full	:1;
	uint32_t rsvd_1			:31;
#endif
#if (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
	uint32_t rsvd_2			:16;
	uint32_t works_accepted		:16;
#else
	uint32_t works_accepted		:16;
	uint32_t rsvd_2			:16;
#endif
	uint32_t rsvd_3;
	struct spi_response responses[MAX_RESPONSES_IN_BATCH];
};
static struct spi_rx_t spi_rxbuf;

struct device_drv knc_drv;
struct active_work {
	struct work *work;
	uint32_t work_id;
	struct timeval begin;
};

struct knc_state {
	struct spidev_context *ctx;
	int devices;
	uint32_t salt;
	uint32_t next_work_id;

	/* read - last read item, next is at (read + 1) mod BUFSIZE
	 * write - next write item, last written at (write - 1) mod BUFSIZE
	 *  When buffer is empty, read + 1 == write
	 *  Buffer full condition: read == write
	 */
	int read_q, write_q;
#define KNC_QUEUED_BUFFER_SIZE	(MAX_REQUESTS_IN_BATCH + 1)
	struct active_work queued_fifo[KNC_QUEUED_BUFFER_SIZE];

	int read_a, write_a;
#define KNC_ACTIVE_BUFFER_SIZE	(MAX_ACTIVE_WORKS + 1)
	struct active_work active_fifo[KNC_ACTIVE_BUFFER_SIZE];

#ifdef ENABLE_PROFILING
	struct timeval last_stats;
#endif

#ifdef ENABLE_BENCHMARK
	struct timeval lastscan;
	struct timeval jupiter_work_start[KNC_ACTIVE_BUFFER_SIZE];
#endif
	uint8_t hwerrs[MAX_ASICS * 256];
};

static inline bool knc_queued_fifo_full(struct knc_state *knc)
{
	return (knc->read_q == knc->write_q);
}

static inline bool knc_active_fifo_full(struct knc_state *knc)
{
	return (knc->read_a == knc->write_a);
}

static inline void knc_queued_fifo_inc_idx(struct knc_state *knc, int *idx)
{
	if (unlikely(*idx >= (KNC_QUEUED_BUFFER_SIZE - 1)))
		*idx = 0;
	else
		++(*idx);
}

static inline void knc_active_fifo_inc_idx(struct knc_state *knc, int *idx)
{
	if (unlikely(*idx >= (KNC_ACTIVE_BUFFER_SIZE - 1)))
		*idx = 0;
	else
		++(*idx);
}

/* Find SPI device with index idx, init it */
static struct spidev_context * spi_new(int idx)
{
	struct spidev_context *ctx;
	char dev_fname[PATH_MAX];

	if(NULL == (ctx = malloc(sizeof(struct spidev_context)))) {
		applog(LOG_ERR, "KnC spi: Out of memory");
		goto l_exit_error;
	}
	ctx->mode = SPI_MODE;
	ctx->bits = SPI_BITS_PER_WORD;
	ctx->speed = SPI_MAX_SPEED;
	ctx->delay = SPI_DELAY_USECS;

	ctx->fd = -1;
#ifdef ENABLE_SPI_HW
	sprintf(dev_fname, SPI_DEVICE_TEMPLATE,
		idx, /* bus */
		0    /* chipselect */
	       );
	if (0 > (ctx->fd = open(dev_fname, O_RDWR))) {
		applog(LOG_ERR, "KnC spi: Can not open SPI device %s: %m",
		       dev_fname);
		goto l_free_exit_error;
	}

	/*
	 * spi mode
	 */
	if (0 > ioctl(ctx->fd, SPI_IOC_WR_MODE, &ctx->mode))
		goto l_ioctl_error;
	if (0 > ioctl(ctx->fd, SPI_IOC_RD_MODE, &ctx->mode))
		goto l_ioctl_error;

	/*
	 * bits per word
	 */
	if (0 > ioctl(ctx->fd, SPI_IOC_WR_BITS_PER_WORD, &ctx->bits))
		goto l_ioctl_error;
	if (0 > ioctl(ctx->fd, SPI_IOC_RD_BITS_PER_WORD, &ctx->bits))
		goto l_ioctl_error;

	/*
	 * max speed hz
	 */
	if (0 > ioctl(ctx->fd, SPI_IOC_WR_MAX_SPEED_HZ, &ctx->speed))
		goto l_ioctl_error;
	if (0 > ioctl(ctx->fd, SPI_IOC_RD_MAX_SPEED_HZ, &ctx->speed))
		goto l_ioctl_error;

	applog(LOG_INFO, "KnC spi: device %s uses mode %hhu, bits %hhu, speed %u",
	       dev_fname, ctx->mode, ctx->bits, ctx->speed);
#endif
	return ctx;

l_ioctl_error:
	applog(LOG_ERR, "KnC spi: ioctl error on SPI device %s: %m", dev_fname);
l_close_free_exit_error:
	close(ctx->fd);
l_free_exit_error:
	free(ctx);
l_exit_error:
	return NULL;
}

static void spi_free(struct spidev_context *ctx)
{
	if (NULL == ctx)
		return;
	close(ctx->fd);
	free(ctx);
}

static int spi_transfer(struct spidev_context *ctx, uint8_t *txbuf,
			uint8_t *rxbuf, int len)
{
	int ret;
	struct spi_ioc_transfer xfr;

	memset(rxbuf, 0xff, len);

	ret = len;
#ifdef ENABLE_SPI_HW
	xfr.tx_buf = (unsigned long)txbuf;
	xfr.rx_buf = (unsigned long)rxbuf;
	xfr.len = len;
	xfr.speed_hz = ctx->speed;
	xfr.delay_usecs = ctx->delay;
	xfr.bits_per_word = ctx->bits;
	xfr.cs_change = 0;
	xfr.pad = 0;

	if (1 > (ret = ioctl(ctx->fd, SPI_IOC_MESSAGE(1), &xfr))) {
		applog(LOG_ERR, "KnC spi xfer: ioctl error on SPI device: %m");
	}
#endif
	return ret;
}

static void disable_core(uint8_t asic, uint8_t core)
{
	char str[256];
	snprintf(str, sizeof(str), "i2cset -y 2 0x2%hhu %hhu 0", asic, core);
	if (0 != WEXITSTATUS(system(str)))
		applog(LOG_ERR, "KnC: system call failed");
}

static int64_t timediff(const struct timeval *a, const struct timeval *b)
{
	struct timeval diff;
	timersub(a, b, &diff);
	return diff.tv_sec * 1000000 + diff.tv_usec;
}

static void knc_work_from_queue_to_spi(struct knc_state *knc,
				       struct active_work *q_work,
				       struct spi_request *spi_req)
{
	uint32_t *buf_from, *buf_to;
	int i;

	spi_req->cmd = CMD_SUBMIT_WORK;
	spi_req->queue_id = 0; /* at the moment we have one and only queue #0 */
	spi_req->work_id = (knc->next_work_id ^ knc->salt) & WORK_ID_MASK;
	q_work->work_id = spi_req->work_id;
	++(knc->next_work_id);
	buf_to = spi_req->midstate;
	buf_from = (uint32_t *)q_work->work->midstate;
	for (i = 0; i < WORK_MIDSTATE_WORDS; ++i)
		buf_to[i] = le32toh(buf_from[8 - i - 1]);
	buf_to = spi_req->data;
	buf_from = (uint32_t *)&(q_work->work->data[16 * 4]);
	for (i = 0; i < WORK_DATA_WORDS; ++i)
		buf_to[i] = le32toh(buf_from[3 - i - 1]);
}

#ifdef ENABLE_PROFILING
extern unsigned long long _tm_total, _tm_merkle, _tm_count, _tm_txns;
#endif

static int64_t knc_process_response(struct thr_info *thr, struct cgpu_info *cgpu,
				    struct spi_rx_t *rxbuf, int num)
{
	struct knc_state *knc = cgpu->knc_state;
	struct work *work;
	int64_t us;
	int works, submitted, completed, i, num_sent;
	int next_read_q, next_read_a;
	struct timeval now;

#ifdef ENABLE_BENCHMARK
#define HASH_RATE		9000
#define WORK_PER_SECOND		(HASH_RATE * 1000 / 4295 + 2)	 /* 1GH/core, 2^32 H/work item (4.295...GH) */
	gettimeofday(&now, NULL);
	us = timediff(&now, &knc->lastscan);
	if (us < 0) {
		knc->lastscan = now;
		return 0;
	}
	works = ((us * WORK_PER_SECOND) / 1000000);

	assert(sizeof(knc->queued_fifo[0]) == sizeof(knc->active_fifo[0]));

	/* move works number of items from queued_fifo to active_fifo */
	submitted = 0;
	for (i = 0; i < works; ++i) {
		next_read_q = knc->read_q;
		knc_queued_fifo_inc_idx(knc, &next_read_q);
		if ((next_read_q == knc->write_q) || knc_active_fifo_full(knc))
			break;
		memcpy(&knc->active_fifo[knc->write_a],
		       &knc->queued_fifo[next_read_q],
		       sizeof(struct active_work));
		knc->jupiter_work_start[knc->write_a] = now;
		knc->queued_fifo[next_read_q].work = NULL;
		knc->read_q = next_read_q;
		knc_active_fifo_inc_idx(knc, &knc->write_a);
		++submitted;
	}
	if (submitted > 0) {
		struct timeval diff;
		diff.tv_sec = 0;
		diff.tv_usec = us - (works * 1000000) / WORK_PER_SECOND;
		timersub(&now, &diff, &knc->lastscan);
	}

	/* check for completed works */
	completed = 0;
	next_read_a = knc->read_a;
	knc_active_fifo_inc_idx(knc, &next_read_a);
	while (next_read_a != knc->write_a) {
		us = timediff(&now, &knc->jupiter_work_start[next_read_a]);
		if ((us >= 0) && (us < 4000000))
			break;
		knc->read_a = next_read_a;
		work = knc->active_fifo[next_read_a].work;
		if (NULL != thr)
			submit_nonce(thr, work, 0);
		work_completed(cgpu, work);
		knc->active_fifo[next_read_a].work = NULL;
		knc_active_fifo_inc_idx(knc, &next_read_a);
		++completed;
	}
#else
	if (knc->write_q > knc->read_q)
		num_sent = knc->write_q - knc->read_q - 1;
	else
		num_sent =
			knc->write_q + KNC_QUEUED_BUFFER_SIZE - knc->read_q - 1;
	/* Actually process SPI response */
	if (rxbuf->works_accepted) {
		applog(LOG_DEBUG, "KnC spi: raw response %08X %08X",
		       ((uint32_t *)rxbuf)[0], ((uint32_t *)rxbuf)[1]);
		applog(LOG_DEBUG,
		       "KnC spi: response, accepted %u (from %u), full %u",
		       rxbuf->works_accepted, num_sent,
		       rxbuf->response_queue_full);
	}
	/* move works_accepted number of items from queued_fifo to active_fifo */
	gettimeofday(&now, NULL);
	submitted = 0;
	for (i = 0; i < rxbuf->works_accepted; ++i) {
		next_read_q = knc->read_q;
		knc_queued_fifo_inc_idx(knc, &next_read_q);
		if ((next_read_q == knc->write_q) || knc_active_fifo_full(knc))
			break;
		memcpy(&knc->active_fifo[knc->write_a],
		       &knc->queued_fifo[next_read_q],
		       sizeof(struct active_work));
		knc->active_fifo[knc->write_a].begin = now;
		knc->queued_fifo[next_read_q].work = NULL;
		knc->read_q = next_read_q;
		knc_active_fifo_inc_idx(knc, &knc->write_a);
		++submitted;
	}
	if (submitted != rxbuf->works_accepted)
		applog(LOG_ERR,
		       "KnC: accepted by FPGA %u works, but only %d submitted",
		       rxbuf->works_accepted, submitted);

	/* check for completed works and calculated nonces */
	gettimeofday(&now, NULL);
	completed = 0;
	for (i = 0; i < MAX_RESPONSES_IN_BATCH; ++i)
	{
		if ( (rxbuf->responses[i].type != RESPONSE_TYPE_NONCE_FOUND) &&
		     (rxbuf->responses[i].type != RESPONSE_TYPE_WORK_DONE)
		   )
			continue;
		applog(LOG_DEBUG, "KnC spi: raw response %08X %08X",
		       ((uint32_t *)&rxbuf->responses[i])[0],
		       ((uint32_t *)&rxbuf->responses[i])[1]);
		applog(LOG_DEBUG, "KnC spi: response, T:%u C:%u-%u Q:%u W:%u",
		       rxbuf->responses[i].type,
		       rxbuf->responses[i].asic, rxbuf->responses[i].core,
		       rxbuf->responses[i].queue_id,
		       rxbuf->responses[i].work_id);
		/* Find active work with matching ID */
		next_read_a = knc->read_a;
		knc_active_fifo_inc_idx(knc, &next_read_a);
		while (next_read_a != knc->write_a) {
			if (knc->active_fifo[next_read_a].work_id ==
			    rxbuf->responses[i].work_id)
				break;

			/* check for stale works */
			us = timediff(&now,
				      &knc->active_fifo[next_read_a].begin);
			if ((us < 0) || (us >= WORK_STALE_US)) {
				applog(LOG_DEBUG,
				       "KnC spi: remove stale work %u",
				       knc->active_fifo[next_read_a].work_id);
				work = knc->active_fifo[next_read_a].work;
				knc_active_fifo_inc_idx(knc, &knc->read_a);
				work_completed(cgpu, work);
				if (next_read_a != knc->read_a)
					memcpy(&(knc->active_fifo[next_read_a]),
					       &(knc->active_fifo[knc->read_a]),
					       sizeof(struct active_work));
				knc->active_fifo[knc->read_a].work = NULL;
			}

			knc_active_fifo_inc_idx(knc, &next_read_a);
		}
		if (next_read_a == knc->write_a)
			continue;
		applog(LOG_DEBUG, "KnC spi: response work %u found",
		       rxbuf->responses[i].work_id);
		work = knc->active_fifo[next_read_a].work;

		if (rxbuf->responses[i].type == RESPONSE_TYPE_NONCE_FOUND) {
			if (NULL != thr) {
				int cidx = rxbuf->responses[i].asic * 256 +
					   rxbuf->responses[i].core;
				if (submit_nonce(thr, work,
						 rxbuf->responses[i].nonce)) {
					if (cidx < sizeof(knc->hwerrs))
						knc->hwerrs[cidx] = 0;
				} else  {
					if (cidx < sizeof(knc->hwerrs)) {
						if (++(knc->hwerrs[cidx]) >= HW_ERR_LIMIT) {
						    disable_core(rxbuf->responses[i].asic, rxbuf->responses[i].core);
							applog(LOG_WARNING,
		"KnC: core %u-%u was disabled due to %u HW errors in a row",
		rxbuf->responses[i].asic,rxbuf->responses[i].core,HW_ERR_LIMIT);
						}
					}
				};
			}
			continue;
		}

		/* Work completed */
		knc_active_fifo_inc_idx(knc, &knc->read_a);
		work_completed(cgpu, work);
		if (next_read_a != knc->read_a)
			memcpy(&(knc->active_fifo[next_read_a]),
			       &(knc->active_fifo[knc->read_a]),
			       sizeof(struct active_work));
		knc->active_fifo[knc->read_a].work = NULL;
		++completed;
	}
#endif

#ifdef ENABLE_PROFILING
	struct timeval last_stats;
	gettimeofday(&now, NULL);
	us = timediff(&now, &knc->last_stats);
	if (us < 0) {
		knc->last_stats = now;
	} else if (us >= 10000000) {
		if (_tm_count == 0)
			_tm_count = -1;
		applog(LOG_ERR, "KnC: stats T: %llu, M: %llu, W/s: %lld, TR: %llu",
		       (_tm_total / _tm_count) / 1000,
		       (_tm_merkle / _tm_count) / 1000,
		       (long long)_tm_count / 10,
			_tm_txns);
		_tm_total = _tm_merkle = _tm_count = 0;
		knc->last_stats = now;
	}
#endif

	return ((uint64_t)completed) * 0x100000000UL;
}

/* Send flush command via SPI */
static int _internal_knc_flush_fpga(struct knc_state *knc)
{
	int len;

	spi_txbuf[0].cmd = CMD_FLUSH_QUEUE;
	spi_txbuf[0].queue_id = 0; /* at the moment we have one and only queue #0 */
	len = spi_transfer(knc->ctx, (uint8_t *)spi_txbuf,
			   (uint8_t *)&spi_rxbuf, sizeof(struct spi_request));
	if (len != sizeof(struct spi_request))
		return -1;
	len /= sizeof(struct spi_response);

	return len;
}

static bool knc_selftest(struct spidev_context *ctx, int devices)
{
	if (!DO_SELFTEST)
		return true;

	assert(sizeof(struct spi_request) == 48);
	assert(sizeof(struct spi_response) == 12);
	assert(sizeof(spi_rxbuf) == ((MAX_RESPONSES_IN_BATCH + 1) * 12));

	return true;
}

static bool knc_detect_one(struct spidev_context *ctx)
{
	/* Scan device for ASICs */
	int chip_id;
	int devices = 0;

	for (chip_id = 0; chip_id < MAX_ASICS; ++chip_id) {
		/* TODO: perform the ASIC test/detection */
		++devices;
	}

	if (!devices) {
		applog(LOG_INFO, "SPI detected, but not KnCminer ASICs");
		return false;
	}
	applog(LOG_INFO, "Found a KnC miner with %d ASICs", devices);

	struct cgpu_info *cgpu = calloc(1, sizeof(*cgpu));
	struct knc_state *knc = calloc(1, sizeof(*knc));
	if (!cgpu || !knc) {
		applog(LOG_ERR, "KnC miner detected, but failed to allocate memory");
		return false;
	}

	knc->ctx = ctx;
	knc->devices = devices;
	knc->read_q = 0;
	knc->write_q = 1;
	knc->read_a = 0;
	knc->write_a = 1;
	knc->salt = rand();
#ifdef ENABLE_BENCHMARK
	gettimeofday(&knc->lastscan, NULL);
#endif

	_internal_knc_flush_fpga(knc);

	knc_selftest(ctx, devices);

	cgpu->drv = &knc_drv;
	cgpu->name = "KnCminer";
	cgpu->threads = 1;	// .. perhaps our number of devices?

	cgpu->knc_state = knc;
	add_cgpu(cgpu);

	return true;
}

// http://www.concentric.net/~Ttwang/tech/inthash.htm
static unsigned long mix(unsigned long a, unsigned long b, unsigned long c)
{
    a=a-b;  a=a-c;  a=a^(c >> 13);
    b=b-c;  b=b-a;  b=b^(a << 8);
    c=c-a;  c=c-b;  c=c^(b >> 13);
    a=a-b;  a=a-c;  a=a^(c >> 12);
    b=b-c;  b=b-a;  b=b^(a << 16);
    c=c-a;  c=c-b;  c=c^(b >> 5);
    a=a-b;  a=a-c;  a=a^(c >> 3);
    b=b-c;  b=b-a;  b=b^(a << 10);
    c=c-a;  c=c-b;  c=c^(b >> 15);
    return c;
}

/* Probe devices and register with add_cgpu */
void knc_detect(void)
{
	int idx;

	srand(mix(clock(), time(NULL), getpid()));

	/* Loop through all possible SPI interfaces */
	for (idx = 0; idx < MAX_SPIS; ++idx) {
		struct spidev_context *ctx = spi_new(idx + 1);
		if (ctx != NULL) {
			if (!knc_detect_one(ctx))
				spi_free(ctx);
		}
	}
}

/* return value is number of nonces that have been checked since
 * previous call
 */
static int64_t knc_scanwork(struct thr_info *thr)
{
	struct cgpu_info *cgpu = thr->cgpu;
	struct knc_state *knc = cgpu->knc_state;
	int len, num;
	int next_read_q;

	applog(LOG_DEBUG, "KnC running scanwork");

	/* Prepare tx buffer */
	memset(spi_txbuf, 0, sizeof(spi_txbuf));
	num = 0;
	next_read_q = knc->read_q;
	knc_queued_fifo_inc_idx(knc, &next_read_q);
	while (next_read_q != knc->write_q) {
		knc_work_from_queue_to_spi(knc, &knc->queued_fifo[next_read_q],
					   &spi_txbuf[num]);
		knc_queued_fifo_inc_idx(knc, &next_read_q);
		++num;
	}
	/* knc->read_q is advanced in knc_process_response, not here */

	len = spi_transfer(knc->ctx, (uint8_t *)spi_txbuf,
			   (uint8_t *)&spi_rxbuf, sizeof(spi_txbuf));
	if (len != sizeof(spi_rxbuf))
		return -1;
	len /= sizeof(struct spi_response);

	applog(LOG_DEBUG, "KnC spi: %d works in request", num);

	return knc_process_response(thr, cgpu, &spi_rxbuf, len);
}

static bool knc_queue_full(struct cgpu_info *cgpu)
{
	struct knc_state *knc = cgpu->knc_state;
	struct work *work;
	int queue_full = true;

	applog(LOG_DEBUG, "KnC running queue full");
	while (!knc_queued_fifo_full(knc)) {
		work = get_queued(cgpu);
		if (!work) {
			queue_full = false;
			break;
		}
		knc->queued_fifo[knc->write_q].work = work;
		knc_queued_fifo_inc_idx(knc, &(knc->write_q));
	}

	return queue_full;
}

static void knc_flush_work(struct cgpu_info *cgpu)
{
	struct knc_state *knc = cgpu->knc_state;
	struct work *work;
	int len;
	int next_read_q, next_read_a;

	applog(LOG_ERR, "KnC running flushwork");

	/* Drain queued works */
	next_read_q = knc->read_q;
	knc_queued_fifo_inc_idx(knc, &next_read_q);
	while (next_read_q != knc->write_q) {
		work = knc->queued_fifo[next_read_q].work;
		work_completed(cgpu, work);
		knc->queued_fifo[next_read_q].work = NULL;
		knc->read_q = next_read_q;
		knc_queued_fifo_inc_idx(knc, &next_read_q);
	}

	/* Drain active works */
	next_read_a = knc->read_a;
	knc_active_fifo_inc_idx(knc, &next_read_a);
	while (next_read_a != knc->write_a) {
		work = knc->active_fifo[next_read_a].work;
		work_completed(cgpu, work);
		knc->active_fifo[next_read_a].work = NULL;
		knc->read_a = next_read_a;
		knc_active_fifo_inc_idx(knc, &next_read_a);
	}

	len = _internal_knc_flush_fpga(knc);
	if (len > 0)
		knc_process_response(NULL, cgpu, &spi_rxbuf, len);
}

struct device_drv knc_drv = {
	.drv_id = DRIVER_KNC,
	.dname = "KnCminer",
	.name = "KnC",
	.drv_detect = knc_detect,	// Probe for devices, add with add_cgpu

	.hash_work = hash_queued_work,
	.scanwork = knc_scanwork,
	.queue_full = knc_queue_full,
	.flush_work = knc_flush_work,
};
