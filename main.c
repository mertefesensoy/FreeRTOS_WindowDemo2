#include <stdio.h>
#include <string.h>

#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>

#define USE_MUTEX 1

/* ---------- Task priorities ---------- */
#define PRIO_LOW        (tskIDLE_PRIORITY + 1)   /* L */
#define PRIO_MEDIUM     (tskIDLE_PRIORITY + 2)   /* M */
#define PRIO_HIGH       (tskIDLE_PRIORITY + 3)   /* H */

/* ---------- Timing knobs (tune if needed) ---------- */
#define L_REPEAT_PERIOD_MS      3000   /* How often L does a long “resource use” */
#define H_START_DELAY_MS         150   /* H tries a bit after L starts */
#define M_BURST_SLICE_ITER     20000   /* “Busy work” iterations per slice */
#define M_BURST_CYCLES            50   /* How many slices per burst before yielding */
#define HOLD_DELAY_PER_CHAR_MS     10  /* Makes L hold lock longer per printed char */

#if USE_MUTEX
static SemaphoreHandle_t xResLock;     /* actually a mutex */
#else
static SemaphoreHandle_t xResLock;     /* binary semaphore */
#endif

/* Utility: timestamped print */
static void logf(const char* tag, const char* fmt, ...)
{
    TickType_t now = xTaskGetTickCount();
    unsigned ms = (unsigned)pdTICKS_TO_MS(now);
    //printf("[%8u ms] %-2s | ", ms, tag);
    va_list ap; va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

/* Simulated “resource”: print a string char-by-char WHILE holding the lock */
static void use_shared_resource(const char* who, const char* msg)
{
    /* Expect lock is already taken by caller */
    for (const char* p = msg; *p; ++p)
    {
        putchar(*p);
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(HOLD_DELAY_PER_CHAR_MS+100));
    }
    putchar('\n'); 
    fflush(stdout);
}

/* ------------------ Task L (Low) ------------------ */
static void vTaskL(void* pv)
{
    (void)pv;
    const char* bigMsg = "L: Using the resource very SLOWLY (simulating long critical section) ...";
    for (;;)
    {
        //logf("L", "Attempting to take lock...");
        if (xSemaphoreTake(xResLock, portMAX_DELAY) == pdPASS)
        {
            logf("L", "Got lock, starting long use.");
            vTaskDelay(pdMS_TO_TICKS(100));
            /* Keep the lock WHILE doing slow prints (this is “bad” on purpose) */
            use_shared_resource("L", bigMsg);
            logf("L", "Releasing lock.");
            xSemaphoreGive(xResLock);
        }
        /* Do it again later */
        vTaskDelay(pdMS_TO_TICKS(L_REPEAT_PERIOD_MS));
    }
}

/* ------------------ Task H (High) ------------------ */
static void vTaskH(void* pv)
{
    (void)pv;
    /* Start slightly later than L so L likely holds the resource */
    vTaskDelay(pdMS_TO_TICKS(H_START_DELAY_MS));
    for (;;)
    {
        //logf("H", "Needs resource; trying to take lock...");
        TickType_t t0 = xTaskGetTickCount();
        if (xSemaphoreTake(xResLock, portMAX_DELAY) == pdPASS)
        {
            TickType_t t1 = xTaskGetTickCount();
            logf("H", "Acquired lock after %u ms wait.",
                (unsigned)pdTICKS_TO_MS(t1 - t0));

            /* Quick use, then release */
            use_shared_resource("H", "H: quick critical section done.");
            xSemaphoreGive(xResLock);
            logf("H", "Released lock, work complete.");

            /* Wait a while so we see repeated cycles */
            vTaskDelay(pdMS_TO_TICKS(1500));
        }
    }
}

/* ------------------ Task M (Medium) ------------------ */
/* Does bursts of CPU work (not using the resource), to preempt L while H is blocked */
static void vTaskM(void* pv)
{
    (void)pv;
    unsigned burst = 0;
    volatile unsigned sink = 0;

    for (;;)
    {
        /* “Busy” for a while, then yield briefly */
        for (unsigned c = 0; c < M_BURST_CYCLES; ++c)
        {
            for (unsigned i = 0; i < M_BURST_SLICE_ITER; ++i)
            {
                sink ^= (i ^ sink); /* useless math to burn CPU */
            }
            /* keep runnable; a tiny delay still keeps M dominant over L */
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        //logf("M", "Finished a CPU burst #%u (not using resource).", ++burst);

        /* Tiny pause between bursts */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void create_lock(void)
{
#if USE_MUTEX
    xResLock = xSemaphoreCreateMutex();
    configASSERT(xResLock != NULL);
    logf("SYS", "Using MUTEX (priority inheritance ENABLED).");
#else
    xResLock = xSemaphoreCreateBinary();
    configASSERT(xResLock != NULL);
    /* Binary semaphores start empty; give once so it behaves like “unlocked”. */
    xSemaphoreGive(xResLock);
    logf("SYS", "Using BINARY SEMAPHORE (NO priority inheritance).");
#endif
}

int main(void)
{
    printf("\n=== FreeRTOS Priority Inversion Demo (USE_MUTEX=%d) ===\n", USE_MUTEX);

    create_lock();

    /* Create tasks: L lowest, M middle, H highest */
    BaseType_t ok = pdPASS;
    ok &= xTaskCreate(vTaskL, "L", configMINIMAL_STACK_SIZE + 512, NULL, PRIO_LOW, NULL);
    ok &= xTaskCreate(vTaskM, "M", configMINIMAL_STACK_SIZE + 512, NULL, PRIO_MEDIUM, NULL);
    ok &= xTaskCreate(vTaskH, "H", configMINIMAL_STACK_SIZE + 512, NULL, PRIO_HIGH, NULL);
    configASSERT(ok == pdPASS);

    vTaskStartScheduler();

    /* Should never return */
    for (;;);
    return 0;
}
