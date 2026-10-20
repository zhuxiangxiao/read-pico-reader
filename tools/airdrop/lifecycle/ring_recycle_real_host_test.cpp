/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：R-05 流式 ring 中断回收的本地主机回归；真实 ad_port_esp32.cpp + 平台桩。
 * English: Local host regression for R-05 streaming ring recycling; real ad_port_esp32.cpp with platform stubs.
 */
/* Host test for R-05: the streaming ring's abort paths must recycle every block.
 *
 * 中文：R-05 主机回归。用 /tmp 级的 IDF/FreeRTOS/mbedTLS 桩头文件把
 * components/read_pico_airdrop/airdrop/port/ad_port_esp32.cpp 原样编进本 TU，
 * 直接调用真实的 stream_infra_alloc / stream_produce / stream_producer_end /
 * stream_consumer_poll，断言 4 个 64 KB 环块在每次中断后仍恰好归属一处。
 *
 * English: R-05 host regression. The REAL ad_port_esp32.cpp is included in this TU
 * against stub IDF/FreeRTOS/mbedTLS headers (syntax only for the unused surfaces; the
 * Mach-O dead-strip link drops them), so the assertions run against the shipped ring
 * logic, not a re-implementation: stream_infra_alloc / stream_produce /
 * stream_producer_end / stream_consumer_poll are called directly and every 64 KiB ring
 * block must stay accounted after each aborted transfer.
 *
 * Run from the repository root:
 *   g++ -std=c++17 -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
 *       -Wno-deprecated-declarations -Wno-unused-function \
 *       -Ibuild/review/airdrop_port_stubs build/review/ring_recycle_real_host_test.cpp \
 *       -o build/review/ring_recycle_real_host_test -Wl,-dead_strip
 *   ./build/review/ring_recycle_real_host_test
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <deque>
#include <vector>

#include "ad_port_esp32.cpp"

/* ---- FreeRTOS stubs with real queue semantics ---- */
struct StubQueue {
    size_t item_size = 0;
    size_t cap = 0;
    std::deque<std::vector<unsigned char>> items;
};

static bool g_clock_stopped = false;
static int64_t g_fake_time_us = 0;

/* Non-blocking stubs: a wait is bounded by the caller's own timeout loop, and the fake
 * clock below moves 50 ms per read so every wait loop terminates deterministically.
 * 非阻塞桩 + 每读一次前进 50ms 的假时钟，保证等待循环确定性退出。 */
extern "C" {
QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item_size) {
    StubQueue *q = new StubQueue();
    q->item_size = (size_t)item_size;
    q->cap = (size_t)len;
    return (QueueHandle_t)q;
}
BaseType_t xQueueSend(QueueHandle_t h, const void *item, TickType_t) {
    StubQueue *q = (StubQueue *)h;
    if (!q || q->items.size() >= q->cap) return pdFALSE;
    std::vector<unsigned char> v(q->item_size);
    memcpy(v.data(), item, q->item_size);
    q->items.push_back(std::move(v));
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t h, void *item, TickType_t) {
    StubQueue *q = (StubQueue *)h;
    if (!q || q->items.empty()) return pdFALSE;
    memcpy(item, q->items.front().data(), q->item_size);
    q->items.pop_front();
    return pdTRUE;
}
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t h) {
    StubQueue *q = (StubQueue *)h;
    return q ? (UBaseType_t)q->items.size() : 0;
}
void vQueueDelete(QueueHandle_t h) { delete (StubQueue *)h; }
void vTaskDelay(TickType_t) {}
void vTaskDelete(TaskHandle_t) {}
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t) { return 0; }
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t, const char *, uint32_t, void *, UBaseType_t,
                                   TaskHandle_t *, BaseType_t) { return pdPASS; }
TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t, const char *, uint32_t, void *,
                                           UBaseType_t, StackType_t *, StaticTask_t *,
                                           BaseType_t) { return (TaskHandle_t)1; }
int64_t esp_timer_get_time(void) {
    if (!g_clock_stopped) g_fake_time_us += 50000;   /* 50 ms per read / 50 ms per read */
    return g_fake_time_us;
}
void *heap_caps_malloc(size_t n, uint32_t) { return malloc(n); }
void *heap_caps_calloc(size_t n, size_t sz, uint32_t) { return calloc(n, sz); }
void *heap_caps_realloc(void *p, size_t n, uint32_t) { return realloc(p, n); }
void heap_caps_free(void *p) { free(p); }
size_t heap_caps_get_free_size(uint32_t) { return 8u * 1024u * 1024u; }
size_t heap_caps_get_largest_free_block(uint32_t) { return 8u * 1024u * 1024u; }
uint32_t esp_get_free_internal_heap_size(void) { return 100u * 1024u; }
}  /* extern "C" */

/* ---- helpers ---- */
static int g_failures = 0;

static void check(bool ok, const char *what) {
    if (!ok) {
        printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

static unsigned free_blocks(void) { return (unsigned)uxQueueMessagesWaiting(g_sfree_q); }
static unsigned ready_blocks(void) { return (unsigned)uxQueueMessagesWaiting(g_sready_q); }

/* The invariant the fix exists for: every ring block is in exactly one place. */
static void check_accounted(const char *when) {
    unsigned free_n = free_blocks();
    unsigned ready_n = ready_blocks();
    unsigned held = g_scur_held ? 1u : 0u;
    if (free_n + ready_n + held != AD_STREAM_BLOCKS) {
        printf("FAIL: %s: blocks free=%u ready=%u held=%u != %u\n", when, free_n, ready_n, held,
               (unsigned)AD_STREAM_BLOCKS);
        ++g_failures;
    }
}

/* No block pointer may appear twice in the free queue (a double release). */
static bool free_queue_has_duplicates(void) {
    StubQueue *q = (StubQueue *)g_sfree_q;
    for (size_t i = 0; i < q->items.size(); ++i) {
        for (size_t j = i + 1; j < q->items.size(); ++j) {
            struct AdStreamBlk a, b;
            memcpy(&a, q->items[i].data(), sizeof(a));
            memcpy(&b, q->items[j].data(), sizeof(b));
            if (a.p != nullptr && a.p == b.p) return true;
        }
    }
    return false;
}

static void drain_and_settle(void) {
    stream_consumer_poll();
}

/* Scenario A (the review's case): a streamed transfer is aborted by the sink while the
 * producer holds a partially filled block. Old code returned with g_scur_held == true and
 * lost one block per abort; four aborts then starved the ring for the rest of the session.
 * 场景 A：消费者已判 sink 失败时生产者手里还有半块；旧代码直接 return 丢掉这一块。 */
static void scenario_abort_while_held(void) {
    static uint8_t data[AD_STREAM_BLOCK_SZ + 100];
    memset(data, 'x', sizeof(data));

    struct RxPump pump;
    memset(&pump, 0, sizeof(pump));
    check(rx_stream_open(&pump), "open stream A");
    check(stream_produce(data, sizeof(data)), "produce 1 full block + 100 B held");
    check(g_scur_held, "one partial block is held while producing");
    check_accounted("A mid-transfer");

    g_stream_sink_failed = true;   /* the consumer's verdict reaches the producer here */
    stream_producer_end();         /* must recycle the held block and mark the stream aborted */
    check(!g_scur_held, "no block is left held after producer end");
    drain_and_settle();
    check(g_stream_done, "consumer finalized without the end marker");
    check_accounted("A after abort");
    check(free_blocks() == AD_STREAM_BLOCKS, "all blocks back in the free queue after A");
    check(!free_queue_has_duplicates(), "no double-released block in the free queue");
}

/* Scenario B: the sink fails earlier, so the first full block cannot even be posted.
 * 场景 B：sink 失败使满块投递也失败，满块必须直接归还。 */
static void scenario_full_block_post_failure(void) {
    /* Fill the ready queue to capacity (markers only), so the next post times out. */
    struct AdStreamBlk marker = {nullptr, 0};
    while (xQueueSend(g_sready_q, &marker, 0) == pdTRUE) {
    }

    struct RxPump pump;
    memset(&pump, 0, sizeof(pump));
    check(rx_stream_open(&pump), "open stream B");
    static uint8_t data[AD_STREAM_BLOCK_SZ];
    memset(data, 'y', sizeof(data));
    check(!stream_produce(data, sizeof(data)), "produce fails when the full block cannot be posted");
    check(!g_scur_held, "failed full block is not left held");
    /* The six queued items here are marker sentinels, not ring blocks, so the generic
     * four-block accounting check cannot be used until they are drained. What matters is
     * that the failed full block went back to the free queue: free is whole again.
     * 这里的 6 个条目是哨兵标记而非环块，先只看空闲队列是否已恢复满员。 */
    check(free_blocks() == AD_STREAM_BLOCKS, "failed full block returned to the free queue");
    check(!free_queue_has_duplicates(), "no double-released block in the free queue after B");

    stream_producer_end();   /* end marker post also fails -> abort flag */
    /* Consume the markers this scenario itself queued. */
    struct AdStreamBlk out;
    while (xQueueReceive(g_sready_q, &out, 0) == pdTRUE) {
    }
    stream_consumer_poll();  /* finalizes from g_stream_abort */
    check(g_stream_done, "consumer finalized from the abort flag after B");
    check_accounted("B after finalize");
    check(free_blocks() == AD_STREAM_BLOCKS, "all blocks back in the free queue after B");
}

/* Scenario C: a normal transfer still works after the aborts, and the ring comes back. */
static void scenario_normal_after_aborts(void) {
    struct RxPump pump;
    memset(&pump, 0, sizeof(pump));
    check(rx_stream_open(&pump), "open stream C");
    static uint8_t small[4096];
    memset(small, 'z', sizeof(small));
    check(stream_produce(small, sizeof(small)), "normal produce after aborts");
    stream_producer_end();
    drain_and_settle();
    check(g_stream_done, "normal transfer finalized");
    check_accounted("C done");
    check(free_blocks() == AD_STREAM_BLOCKS, "ring is whole after a normal transfer");
}

int main(void) {
    check(stream_infra_alloc(), "stream infra alloc");
    check(free_blocks() == AD_STREAM_BLOCKS, "ring starts with every block free");

    /* The review's failure signature: four aborted transfers in one session. */
    for (int i = 0; i < 4; ++i) {
        scenario_abort_while_held();
        if (g_failures) break;
    }
    scenario_full_block_post_failure();
    scenario_normal_after_aborts();

    stream_infra_free();
    if (g_failures) {
        printf("ring_recycle_real_host_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("ring_recycle_real_host_test: PASS (4 aborts + failed post + normal transfer, "
           "ring always %d blocks)\n", (int)AD_STREAM_BLOCKS);
    return 0;
}
