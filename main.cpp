#include <malloc.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <3ds.h>
#include <cstdarg>

static PrintConsole topScreen, bottomScreen;
bool g_log = true;

void InitConsole() {
    gfxInitDefault();

    consoleInit(GFX_TOP, &topScreen);
    consoleInit(GFX_BOTTOM, &bottomScreen);

    consoleSelect(&topScreen);
}

void Log(const char* f, ...) {
    if (!g_log)
        return;
    consoleSelect(&bottomScreen);
    va_list vl;
    va_start(vl, f);
    std::vprintf(f, vl);
    va_end(vl);
    consoleSelect(&topScreen);
}

bool ParseResult(Result r) {
    if (R_SUCCEEDED(r)) {
        Log("\x1b[32mSuccess\x1b[0m\n");
        return true;
    } else {
        //Log("\x1b[31mError: %08lX\x1b[0m\n", r);
        Log("\x1b[31mError:\nLevel: %d\nModule: %d\nSummary: %d\nDescription: %lX\x1b[0m\n", R_LEVEL(r), R_MODULE(r), R_SUMMARY(r), R_DESCRIPTION(r));
        return false;
    }
}

volatile u8* pShared;
u32 offset = 0;
static void PrintSharedMemory() {
    // consoleClear();

    printf("\x1b[0;0H");

    printf("@%03lX", offset);
    for (int i = 0; i < 0x1D0; ++i) {
        if (i % 16 == 0)
            printf("\n");
        printf("%02X ", pShared[i + offset]);
    }
}

static inline int countPrmWords(u32 hdr) {
    return (hdr & 0x3F) + ((hdr >> 6) & 0x3F);
}

std::vector<u32> ExeRequestSilent(Handle session, const std::vector<u32>& request) {
    u32* cmdbuf = getThreadCommandBuffer();
    memcpy(cmdbuf, request.data(), request.size() * 4);
    Result r = svcSendSyncRequest(session);
    if (R_FAILED(r)) {
        return {0, *(u32*)&r};
    }
    int p = countPrmWords(cmdbuf[0]);
    if (p < 3)
        p = 3;
    return std::vector<u32>(cmdbuf, cmdbuf + 1 + p);
}

Handle irHandle;
Handle sharedMemory;
volatile Handle eventRecv = 0;

std::vector<u32> SendIrRequest(const std::vector<u8> &request) {
    return ExeRequestSilent(irHandle, {0x000D0042, request.size(), (request.size() << 14) | 2, (u32)request.data()});
}

void Step() {
    static int step = 0;
    switch (step) {
    case 0:
        Log("InitIrnopShared ");
        ParseResult(ExeRequestSilent(irHandle, {0x00180182, 0x1000, 0x40, 0x2, 0x40, 0x2, 4, 0, (u32)sharedMemory})[1]);
        break;
    case 1: {
        Log("ConnectionStatusEvent ");
        std::vector<u32> response = ExeRequestSilent(irHandle, {0x000C0000});
        ParseResult(response[1]);
        Handle connectionStatusEvent = response[3];

        while (1) {
            hidScanInput();
            u32 kDown = hidKeysDown();
            if (kDown & KEY_START) {
                return;
            }

            Log("RequireConnection ");
            ParseResult(ExeRequestSilent(irHandle, {0x00060040, 1})[1]);
            PrintSharedMemory();
            Log("ConnStatusEvent ");
            int result = svcWaitSynchronization(connectionStatusEvent, 10000000);
            ParseResult(result);
            if (result == 0) {
                if (pShared[8] == 2)
                    break;
            }
            PrintSharedMemory();

            Log("Disconnect ");
            ParseResult(ExeRequestSilent(irHandle, {0x00090000})[1]);
            PrintSharedMemory();

            Log("Waiting for disconnect ");
            ParseResult(svcWaitSynchronization(connectionStatusEvent, 10000000));
            PrintSharedMemory();
        }
        Log("Connected!\n");
        Log("GetReceiveEvent ");
        auto reply = ExeRequestSilent(irHandle, {0x000A0000});
        ParseResult(reply[1]);
        eventRecv = (Handle)reply[3];

        static std::vector<u8> send_buf = {0x01, 0x08, 0x28};

        while (1) {
            hidScanInput();
            u32 kDown = hidKeysDown();
            if (kDown & KEY_START) {
                return;
            }

            Log("SendIrNop(01 10 48) ");
            ParseResult(SendIrRequest(send_buf)[1]);
            PrintSharedMemory();

            Log("RecvEvent ");
            int result = svcWaitSynchronization(eventRecv, 10000000);
            ParseResult(result);
            PrintSharedMemory();
            if (result == 0) {
                break;
            }
        }
        Log("Getting packets!\n");
    } break;
    }
    ++step;
}

int main() {
    aptInit();
    InitConsole();

    // gfxSetDoubleBuffering(GFX_TOP, true);

    Log("Get ir:USER ");
    ParseResult(srvGetServiceHandle(&irHandle, "ir:USER"));

    pShared = (u8*)memalign(0x1000, 0x1000);

    memset((void*)pShared, 0xCC, 0x1000);

    Log("svcCreateMemoryBlock ");

    ParseResult(svcCreateMemoryBlock(&sharedMemory, (u32)pShared, 0x1000, (MemPerm)(MEMPERM_READ),
                                     (MemPerm)(MEMPERM_READ | MEMPERM_WRITE)));

    Handle refresh_timer;
    svcCreateTimer(&refresh_timer, RESET_ONESHOT);
    svcSetTimer(refresh_timer, 0, 100000000);

    u32 kDown;
    while (aptMainLoop()) {
        if (svcWaitSynchronization(refresh_timer, 0) == 0)
            PrintSharedMemory();

        if (eventRecv && svcWaitSynchronization(eventRecv, 0) == 0) {
//            Log("ReleaseReceivedData (recv event) ");
            ExeRequestSilent(irHandle, {0x00190040, 1});
//            PrintSharedMemory();
//            Log("SendIrNop(01 32 D0) ");
            static std::vector<u8> send_buf = {0x01, 0x32, 0xD0};
            SendIrRequest(send_buf);
//            PrintSharedMemory();
        }

        hidScanInput();
        kDown = hidKeysDown();
        switch (kDown) {
        case KEY_A:

            Step();
            break;
        case KEY_START:
            goto EXIT;
        }
        gfxFlushBuffers();
        gspWaitForVBlank();
        gfxSwapBuffers();
    }
EXIT:

    gfxExit();
    aptExit();
    return 0;
}

