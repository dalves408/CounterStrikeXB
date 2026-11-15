/* cs_account.c -- Xbox Live (Insignia) account requests, made with the game's own
 * online library.
 *
 * Accounts are created and recovered by the Dashboard, which is not part of this
 * program. The requests it makes are small: each is an XRL post to the user
 * account service (service 5) at /xuacs/<name>.ashx with a binary body. This
 * makes them itself, through the same library code the game uses to sign in
 * and to ask for content (the game's XONLINE section). Nothing from the
 * Dashboard is used or needed here; it was only studied to learn the requests.
 *
 * Modes (environment):
 *   CS_ACCT=recover  Account recovery, as in Insignia's guide: the gamertag and
 *                    the 16-digit number from the insignia.live web dashboard
 *                    arrive on the pipe whose handle is CS_ACCT_INPUT (Insignia
 *                    Setup makes it; two lines). The account comes back from
 *                    /xuacs/restoreaccount.ashx and is saved to the drive's
 *                    account area the way the Dashboard saves it, signed with
 *                    the console's hard-drive key by the library's own code.
 *                    The number is never logged or written anywhere, and is
 *                    wiped once sent. The process then exits with a result code
 *                    (cs_account.h).
 *   CS_ACCT=reserve, CS_ACCT_NAME=<gamertag>
 *                    Diagnostic: ask whether a gamertag is free
 *                    (/xuacs/reservename.ashx) and log the reply.
 *
 * Both sign the console in with no players and the account service only.
 *
 * Everything runs on a guest thread of its own (cs_account_thread), started
 * from the game's main thread once the online library is up, so no game thread
 * is ever interrupted. Calling into the library from inside a game thread's
 * kernel call deadlocked the game (its audio lock against the IRQL gate).
 *
 * Guest addresses (title update):
 *   0x003A56B8  the library's object (CXo); 0 until XOnlineStartup
 *               +0x1C machine account id (8 bytes), +0x88 config-sector handle
 *   0x002758C1  XOnlineLogon(users[4], services, count, event, &task)
 *   0x002758A0  XOnlineTaskContinue(task): 0 while running, then the result
 *   0x002758AB  XOnlineTaskClose(task)
 *   0x00282146  CXo XRL post (thiscall, 12 arguments, see xrl_post)
 *   0x00272067  config sector read (handle, index, buffer, 0x1EC): nonzero = ok
 *   0x0027211E  config sector write (same arguments)
 *   0x0027A7BE  CXo account-record signer (record, 8-byte confounder)
 *   Config-sector indexes: 3 = the user account directory (8 entries of 12
 *   bytes: in-use flag, confounder), 4..11 = the eight user account slots.
 */
#define _CRT_RAND_S     /* rand_s: the directory entry's confounder */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "cs_account.h"
#include "cs_d3d11.h"   /* cs_now_ms */
#include "cs_diag.h"    /* cs_diag_flush */

uint32_t xbox_HeapAlloc(uint32_t size, uint32_t alignment);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);

#define VA_CXO           0x003A56B8u
#define VA_LOGON         0x002758C1u
#define VA_TASK_CONTINUE 0x002758A0u
#define VA_TASK_CLOSE    0x002758ABu
#define VA_XRL_POST      0x00282146u
#define VA_SECTOR_READ   0x00272067u
#define VA_SECTOR_WRITE  0x0027211Eu
#define VA_RECORD_SIGN   0x0027A7BEu
#define THUNK_KEDELAY    0x0032119Cu
#define THUNK_PSCREATE   0x003211B0u
#define CXO_MACHINE_ID   0x1Cu
#define CXO_CONFIG       0x88u
#define SERVICE_ACCOUNT  5u
#define COUNTRY_US       103u          /* the XONLINE country code for the US */
#define FAKE_RETURN      0xFFFF0000u   /* never returned to: the callee only pops it */
#define SECTOR_DATA      0x1ECu        /* a config sector's data part */
#define RECORD_SIZE      0x6Cu         /* a user account record */
#define REPLY_SIZE       0x400u

enum { MODE_NONE, MODE_RESERVE, MODE_RECOVER };
enum { ST_OFF, ST_WAIT, ST_LOGON, ST_SEND, ST_REPLY, ST_SAVE, ST_DONE };
static int s_mode, s_state = ST_OFF, s_result = CS_ACCT_ERR_OTHER;
static double s_t0;
static volatile LONG s_busy;
static uint32_t s_users, s_services, s_task_slot, s_xrl, s_body, s_resp, s_body_len;
static char s_name[32], s_card[32];

/* ---- input ------------------------------------------------------------------ */
static int read_line(HANDLE h, char *out, unsigned cap)
{
    unsigned n = 0;
    char c;
    DWORD got;
    while (ReadFile(h, &c, 1, &got, NULL) && got == 1) {
        if (c == '\n') break;
        if (c == '\r') continue;
        if (n + 1 < cap) out[n++] = c;
    }
    out[n] = 0;
    return n > 0;
}

/* The two lines Insignia Setup writes: gamertag, then the number (digits only
 * are kept). */
static int read_recover_input(void)
{
    const char *h = getenv("CS_ACCT_INPUT");
    char raw[64];
    HANDLE pipe;
    unsigned i, n = 0;
    if (!h || !*h) return 0;
    pipe = (HANDLE)(uintptr_t)_strtoui64(h, NULL, 10);
    if (!read_line(pipe, s_name, sizeof s_name)) { CloseHandle(pipe); return 0; }
    read_line(pipe, raw, sizeof raw);
    CloseHandle(pipe);
    for (i = 0; raw[i] && n + 1 < sizeof s_card; i++)
        if (raw[i] >= '0' && raw[i] <= '9') s_card[n++] = raw[i];
    s_card[n] = 0;
    SecureZeroMemory(raw, sizeof raw);
    return n >= 12;
}

int cs_account_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *m = getenv("CS_ACCT");
        on = 0;
        if (m && !_stricmp(m, "reserve")) {
            const char *n = getenv("CS_ACCT_NAME");
            if (n && *n && strlen(n) < sizeof s_name) {
                strcpy(s_name, n);
                s_mode = MODE_RESERVE;
                fprintf(stderr, "[ACCT] gamertag check\n");
            }
        } else if (m && !_stricmp(m, "recover")) {
            s_mode = MODE_RECOVER;
            if (!read_recover_input()) {
                fprintf(stderr, "[ACCT] account recovery: no gamertag and number on the input pipe\n");
                s_result = CS_ACCT_ERR_INPUT;
                s_state = ST_DONE;
                cs_account_finish();
            }
            fprintf(stderr, "[ACCT] account recovery for a gamertag of %u characters\n", (unsigned)strlen(s_name));
        }
        if (s_mode != MODE_NONE) { s_state = ST_WAIT; on = 1; }
    }
    return on;
}

/* ---- calling guest code ------------------------------------------------------ */
/* Call a translated guest function (or a kernel export, by the synthetic
 * address its thunk slot holds) the way generated code does: arguments pushed
 * right to left, a return address, ecx for thiscall. Whatever the callee pops,
 * esp is put back afterwards. */
static uint32_t gcall(uint32_t va, uint32_t ecx, int n, const uint32_t *args)
{
    recomp_func_t f = recomp_lookup(va);
    uint32_t esp = g_esp;
    int i;
    if (!f) f = recomp_lookup_manual(va);
    if (!f) f = recomp_lookup_kernel(va);
    if (!f) { fprintf(stderr, "[ACCT] no translated function at %08X\n", va); return 0x80004005u; }
    for (i = n - 1; i >= 0; i--) { g_esp -= 4; MEM32(g_esp) = args[i]; }
    g_esp -= 4; MEM32(g_esp) = FAKE_RETURN;
    g_ecx = ecx;
    f();
    g_esp = esp;
    return g_eax;
}

static uint32_t galloc(uint32_t size)
{
    uint32_t va = xbox_HeapAlloc(size, 16), i;
    for (i = 0; va && i < size; i++) MEM8(va + i) = 0;
    return va;
}

static uint32_t gstring(const char *s)
{
    uint32_t n = (uint32_t)strlen(s) + 1, va = galloc(n), i;
    for (i = 0; va && i < n; i++) MEM8(va + i) = (uint8_t)s[i];
    return va;
}

static void log_hr(const char *what, uint32_t hr)
{
    fprintf(stderr, "[ACCT] %s: 0x%08X\n", what, hr);
}

/* ---- request bodies ------------------------------------------------------------ */
/* A string as the library's writer puts it: 2-byte length, UTF-8 bytes. */
static uint32_t put_string(uint32_t p, const char *s)
{
    uint32_t n = (uint32_t)strlen(s), i;
    MEM8(p) = (uint8_t)n; MEM8(p + 1) = (uint8_t)(n >> 8);
    for (i = 0; i < n; i++) MEM8(p + 2 + i) = (uint8_t)s[i];
    return p + 2 + n;
}

/* Every account request starts with the console's machine account id. */
static uint32_t put_machine_id(uint32_t p, uint32_t cxo)
{
    MEM32(p) = MEM32(cxo + CXO_MACHINE_ID);
    MEM32(p + 4) = MEM32(cxo + CXO_MACHINE_ID + 4);
    return p + 8;
}

static void build_reserve_body(uint32_t cxo)
{
    uint32_t p = put_machine_id(s_body, cxo);
    MEM8(p) = (uint8_t)COUNTRY_US; p += 1;
    MEM8(p) = 5; MEM8(p + 1) = 0; p += 2;               /* names to suggest if taken */
    p = put_string(p, s_name);
    s_body_len = p - s_body;
}

/* The Dashboard's recovery form, in the order its request sends the fields:
 * gamertag, phone area code, phone number, extension, postal code, name, and
 * the card number. Insignia checks the gamertag and its 16-digit number; its
 * guide says to fill the rest with anything, which is what this does. */
static void build_recover_body(uint32_t cxo)
{
    uint32_t p = put_machine_id(s_body, cxo);
    p = put_string(p, s_name);
    p = put_string(p, "555");
    p = put_string(p, "5555555");
    p = put_string(p, "");
    p = put_string(p, "98052");
    p = put_string(p, "Insignia Player");
    p = put_string(p, s_card);
    s_body_len = p - s_body;
    SecureZeroMemory(s_card, sizeof s_card);
}

/* ---- the reply --------------------------------------------------------------- */
/* A failed post leaves the server's headers in the reply buffer; X-Err names
 * the reason. */
static uint32_t reply_error(void)
{
    char t[REPLY_SIZE + 1];
    uint32_t i;
    char *e;
    for (i = 0; i < REPLY_SIZE; i++) { uint8_t c = MEM8(s_resp + i); t[i] = (char)(c ? c : '\n'); }
    t[REPLY_SIZE] = 0;
    e = strstr(t, "X-Err: ");
    return e ? (uint32_t)strtoul(e + 7, NULL, 16) : 0;
}

static void log_reply_headers(void)
{
    uint32_t i;
    fprintf(stderr, "[ACCT] reply: ");
    for (i = 0; i < 200; i++) {
        uint8_t c = MEM8(s_resp + i);
        fputc(c >= 0x20 && c < 0x7F ? c : ' ', stderr);
    }
    fputc('\n', stderr);
}

/* ---- saving a recovered account ------------------------------------------------ */
/* The account record, laid out as the Dashboard builds it from the
 * restoreaccount reply: id (reply +0x00, 8 bytes), gamertag (+0x0C), Kerberos
 * names (reply +0x18, 20 bytes -> +0x24; reply +0x2C, 24 bytes -> +0x38), the
 * account key (reply +0x08, 16 bytes -> +0x50), and the time it was saved
 * (+0x60, two-second units since 2000-01-01, the Dashboard's arithmetic). */
static void build_record(uint32_t rec)
{
    uint32_t i;
    FILETIME ft;
    unsigned long long t;
    for (i = 0; i < 8; i++) MEM8(rec + i) = MEM8(s_resp + i);
    for (i = 0; i < 16 && s_name[i]; i++) MEM8(rec + 0x0C + i) = (uint8_t)s_name[i];
    for (i = 0; i < 20; i++) MEM8(rec + 0x24 + i) = MEM8(s_resp + 0x18 + i);
    for (i = 0; i < 24; i++) MEM8(rec + 0x38 + i) = MEM8(s_resp + 0x2C + i);
    for (i = 0; i < 16; i++) MEM8(rec + 0x50 + i) = MEM8(s_resp + 0x08 + i);
    GetSystemTimeAsFileTime(&ft);
    t = ((unsigned long long)ft.dwHighDateTime << 32 | ft.dwLowDateTime) + 0xFE40A38D010495A0ull;
    MEM32(rec + 0x60) = (uint32_t)(t / 20000000ull);
}

static int sector_io(uint32_t va, uint32_t cxo, uint32_t index, uint32_t buf)
{
    uint32_t a[4] = { MEM32(cxo + CXO_CONFIG), index, buf, SECTOR_DATA };
    return gcall(va, 0, 4, a) != 0;
}

/* The Dashboard's save: the slot already holding this account, else the first
 * free one; record copied in, its flags' high bits and +8 cleared, a fresh
 * confounder in the directory entry, the record signed with it, then the slot
 * and the directory written back. */
static int save_record(uint32_t cxo, uint32_t rec)
{
    uint32_t dir = galloc(SECTOR_DATA), users = galloc(8 * SECTOR_DATA), slot = 8, i;
    if (!dir || !users) return CS_ACCT_ERR_SAVE;
    sector_io(VA_SECTOR_READ, cxo, 3, dir);
    for (i = 0; i < 8; i++)
        if (!sector_io(VA_SECTOR_READ, cxo, 4 + i, users + i * SECTOR_DATA)) {
            uint32_t k;
            for (k = 0; k < SECTOR_DATA; k++) MEM8(users + i * SECTOR_DATA + k) = 0;
        }
    for (i = 0; i < 8 && slot == 8; i++)
        if (MEM32(dir + 12 * i) && MEM32(users + i * SECTOR_DATA) == MEM32(rec)
            && MEM32(users + i * SECTOR_DATA + 4) == MEM32(rec + 4))
            slot = i;
    if (slot < 8) fprintf(stderr, "[ACCT] the account is already in slot %u: replacing it\n", slot);
    for (i = 0; i < 8 && slot == 8; i++)
        if (!MEM32(dir + 12 * i)) slot = i;
    if (slot == 8) return CS_ACCT_ERR_FULL;
    {
        uint32_t ub = users + slot * SECTOR_DATA, de = dir + 12 * slot;
        unsigned int r;
        for (i = 0; i < RECORD_SIZE; i++) MEM8(ub + i) = MEM8(rec + i);
        MEM8(ub + 0x1F) &= 0x0F;
        MEM32(ub + 8) = 0;
        for (i = 0; i < 8; i += 4) { rand_s(&r); MEM32(de + 4 + i) = r; }
        {
            uint32_t a[2] = { ub, de + 4 };
            gcall(VA_RECORD_SIGN, cxo, 2, a);
        }
        MEM32(de) = 1;
        if (!sector_io(VA_SECTOR_WRITE, cxo, 4 + slot, ub) || !sector_io(VA_SECTOR_WRITE, cxo, 3, dir))
            return CS_ACCT_ERR_SAVE;
        for (i = 0; i < SECTOR_DATA; i++) MEM8(ub + i) = 0;          /* the key does not linger */
    }
    fprintf(stderr, "[ACCT] account saved in slot %u\n", slot);
    return CS_ACCT_OK;
}

/* ---- the steps ------------------------------------------------------------------- */
static void step(void)
{
    uint32_t cxo = MEM32(VA_CXO), hr;
    switch (s_state) {
    case ST_WAIT:
        s_users = galloc(4 * 0x70);
        s_services = galloc(16);
        s_task_slot = galloc(16);
        s_xrl = galloc(0x400);
        s_body = galloc(0x200);
        s_resp = galloc(REPLY_SIZE + 16);
        if (!s_users || !s_services || !s_task_slot || !s_xrl || !s_body || !s_resp) {
            fprintf(stderr, "[ACCT] out of guest memory\n"); s_state = ST_DONE; return;
        }
        MEM32(s_services) = SERVICE_ACCOUNT;
        {
            uint32_t a[5] = { s_users, s_services, 1, 0, s_task_slot };
            hr = gcall(VA_LOGON, 0, 5, a);
        }
        log_hr("signing in to Xbox Live (console only, account service)", hr);
        if ((int32_t)hr < 0) { s_result = CS_ACCT_ERR_LOGON; s_state = ST_DONE; return; }
        s_state = ST_LOGON;
        return;
    case ST_LOGON: {
        uint32_t a[1] = { MEM32(s_task_slot) };
        hr = gcall(VA_TASK_CONTINUE, 0, 1, a);
        if (hr == 0) return;                                /* running */
        log_hr("signed in", hr);
        if ((int32_t)hr < 0) { s_result = CS_ACCT_ERR_LOGON; s_state = ST_DONE; return; }
        if (!MEM32(cxo + CXO_MACHINE_ID) && !MEM32(cxo + CXO_MACHINE_ID + 4)) {
            fprintf(stderr, "[ACCT] no machine account after sign-in\n");
            s_result = CS_ACCT_ERR_LOGON; s_state = ST_DONE; return;
        }
        if (s_mode == MODE_RECOVER) build_recover_body(cxo); else build_reserve_body(cxo);
        s_state = ST_SEND;
        return;
    }
    case ST_SEND: {
        uint32_t path = gstring(s_mode == MODE_RECOVER ? "/xuacs/restoreaccount.ashx" : "/xuacs/reservename.ashx");
        /* service, path, reply, reply size, 0, 0, body, body length,
         * timeout (ms), event, task block, service-address override */
        uint32_t a[12] = { SERVICE_ACCOUNT, path, s_resp, REPLY_SIZE, 0, 0, s_body, s_body_len,
                           60000, 0, s_xrl, 0 };
        hr = gcall(VA_XRL_POST, cxo, 12, a);
        log_hr("request sent", hr);
        if ((int32_t)hr < 0) { s_result = CS_ACCT_ERR_NETWORK; s_state = ST_DONE; return; }
        s_state = ST_REPLY;
        return;
    }
    case ST_REPLY: {
        uint32_t a[1] = { s_xrl }, i;
        hr = gcall(VA_TASK_CONTINUE, 0, 1, a);
        if (hr == 0) return;
        for (i = 0; i < s_body_len; i++) MEM8(s_body + i) = 0;      /* the number does not linger */
        log_hr("reply", hr);
        if ((int32_t)hr < 0) {
            uint32_t err = reply_error();
            fprintf(stderr, "[ACCT] the server refused the request (X-Err %08X)\n", err);
            if (s_mode == MODE_RESERVE) log_reply_headers();
            s_result = err ? CS_ACCT_ERR_REFUSED : CS_ACCT_ERR_NETWORK;
            s_state = ST_DONE;
            return;
        }
        if (s_mode == MODE_RESERVE) { log_reply_headers(); s_result = CS_ACCT_OK; s_state = ST_DONE; return; }
        s_state = ST_SAVE;
        return;
    }
    case ST_SAVE: {
        uint32_t rec = galloc(RECORD_SIZE), i;
        build_record(rec);
        for (i = 0; i < REPLY_SIZE; i++) MEM8(s_resp + i) = 0;
        s_result = rec ? save_record(cxo, rec) : CS_ACCT_ERR_SAVE;
        for (i = 0; rec && i < RECORD_SIZE; i++) MEM8(rec + i) = 0;
        s_state = ST_DONE;
        return;
    }
    default:
        return;
    }
}

/* Recovery is all this run is for: report and end the process. The emulated
 * drive has already written the account area to its file. */
void cs_account_finish(void)
{
    if (s_mode != MODE_RECOVER) return;
    fprintf(stderr, "[ACCT] account recovery finished: result %d\n", s_result);
    cs_diag_flush();                   /* the log file gets every line before the process ends */
    TerminateProcess(GetCurrentProcess(), (UINT)s_result);
}

/* The account thread: a guest thread of its own (PsCreateSystemThreadEx with
 * CS_ACCOUNT_THREAD_VA as its routine, which cs_kernel.c maps here). Between
 * steps it waits with the guest's KeDelayExecutionThread, which lets the other
 * guest threads (the network stack among them) run. */
void cs_account_thread(void)
{
    uint32_t interval = galloc(8);
    double start = cs_now_ms();
    fprintf(stderr, "[ACCT] account thread running\n");
    MEM32(interval) = (uint32_t)-500000; MEM32(interval + 4) = 0xFFFFFFFFu;   /* 50 ms, relative */
    while (s_state != ST_DONE) {
        uint32_t a[3] = { 0, 0, interval };
        step();
        if (s_state != ST_DONE && cs_now_ms() - start > 120000.0) {
            fprintf(stderr, "[ACCT] no answer in 2 minutes\n");
            s_result = CS_ACCT_ERR_NETWORK;
            s_state = ST_DONE;
        }
        gcall(MEM32(THUNK_KEDELAY), 0, 3, a);
    }
    fprintf(stderr, "[ACCT] account thread done (result %d)\n", s_result);
    cs_account_finish();
    g_eax = 0;                         /* the bridge pops what it pushed */
}

static int s_thread_started;

/* From the game's KeQuerySystemTime calls: once the game has started its
 * online library, start the account thread (from the main thread at normal
 * IRQL, out of the game's own code). */
void cs_account_tick(void)
{
    if (s_state != ST_WAIT || s_thread_started) return;
    if (s_t0 == 0) s_t0 = cs_now_ms();
    if (!MEM32(VA_CXO) || cs_now_ms() - s_t0 < 20000.0) return;
    if (!g_fs_base || MEM8(g_fs_base + 0x24) != 0) return;
    if (g_esp < 0x00890000u || g_esp >= 0x01090000u) return;
    if (MEM32(g_esp) < 0x00012000u || MEM32(g_esp) >= 0x00240260u) return;
    if (InterlockedCompareExchange(&s_busy, 1, 0)) return;
    s_thread_started = 1;
    {
        uint32_t r[9], handle = galloc(8), tid = galloc(8), hr;
        r[0] = g_eax; r[1] = g_ecx; r[2] = g_edx; r[3] = g_ebx; r[4] = g_esp;
        r[5] = g_ebp; r[6] = g_esi; r[7] = g_edi; r[8] = g_eflags;
        g_esp -= 64;
        {
            /* handle, extension size, kernel stack, TLS size, thread id,
             * start routine, start context, suspended, debugger, system routine */
            uint32_t a[10] = { handle, 0, 0, 0, tid, 0, 0, 0, 0, CS_ACCOUNT_THREAD_VA };
            hr = gcall(MEM32(THUNK_PSCREATE), 0, 10, a);
        }
        g_eax = r[0]; g_ecx = r[1]; g_edx = r[2]; g_ebx = r[3]; g_esp = r[4];
        g_ebp = r[5]; g_esi = r[6]; g_edi = r[7]; g_eflags = r[8];
        log_hr("account thread created", hr);
    }
    InterlockedExchange(&s_busy, 0);
}
