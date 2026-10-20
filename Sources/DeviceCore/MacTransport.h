#ifndef SEKONIC_MAC_TRANSPORT_H
#define SEKONIC_MAC_TRANSPORT_H
#include "LanguageCore.h"
/* Allocates local bookkeeping only. No enumeration/open/query until sl_run. */
bool sl_mac_create(SLTransport *transport);
void sl_mac_destroy(SLTransport *transport);
#endif
