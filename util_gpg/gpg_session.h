#ifndef GPG_SESSION_H
#define GPG_SESSION_H

#include <gpgme.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

//int gpgsession_add_key(struct gpgsession *session, gpgme_key_t key);
void gpgsession_new(gpgme_ctx_t *ctx, bool ephemeral);
int gpgsession_gather_secret_keys(gpgme_ctx_t *ctx);
int gpgsession_add_data(gpgme_ctx_t * const ctx, const char * const data, const size_t length);

#endif
