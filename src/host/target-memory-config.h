/* Experimental mapping budgets, not an established EL2 capacity limit. */
#ifndef TARGET_MEMORY_CONFIG_H
#define TARGET_MEMORY_CONFIG_H
#ifdef TARGET_MIXED_FOLIOS
#define TARGET_MIN_FOLIO_SIZE 65536
/* Crash isolation: exclude PMD-sized donations; device cause still unproven. */
#define TARGET_MAX_FOLIO_SIZE (1024u * 1024u)
/* Small resident pages are valid backing; bound metadata by folio count. */
#define TARGET_MAX_FOLIOS 131072u
#endif
#endif
