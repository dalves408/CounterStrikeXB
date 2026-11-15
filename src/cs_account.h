#pragma once
/* Xbox Live (Insignia) account requests made with the game's own online
 * library (cs_account.c). */
int  cs_account_enabled(void);   /* CS_ACCT=recover (or reserve) */
void cs_account_tick(void);      /* from a game thread's kernel call: starts the account thread */
void cs_account_thread(void);    /* the account thread's routine (a guest thread) */
void cs_account_finish(void);    /* recovery: end the process with the result code */

/* The synthetic guest address the account thread starts at; cs_kernel.c maps
 * it to cs_account_thread. Kernel-thunk range, so the override table sees it. */
#define CS_ACCOUNT_THREAD_VA 0xFEACC000u

/* Exit codes of a recovery run (CS_ACCT=recover), read by Insignia Setup. */
#define CS_ACCT_OK           0
#define CS_ACCT_ERR_INPUT    20   /* no gamertag and number on the input pipe */
#define CS_ACCT_ERR_LOGON    21   /* the console could not sign in (identity, network) */
#define CS_ACCT_ERR_NETWORK  22   /* no answer from the account service */
#define CS_ACCT_ERR_REFUSED  23   /* the service refused: wrong gamertag or number */
#define CS_ACCT_ERR_SAVE     24   /* the account could not be written to the drive */
#define CS_ACCT_ERR_FULL     25   /* all eight account slots are in use */
#define CS_ACCT_ERR_OTHER    26
