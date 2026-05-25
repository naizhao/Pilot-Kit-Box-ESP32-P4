/*
 * icao_country.c — ICAO 24-bit address → country lookup table.
 *
 * Source: ICAO Annex 10 Vol III (Doc 7910) — "Allocations of SSR Mode S
 * Aircraft Addresses". Curated subset chosen for:
 *   1. China (0x780000-0x7BFFFF) — user's primary coverage area
 *   2. East / Southeast Asia neighbours (cross-border traffic)
 *   3. Global top sources by aircraft count (US, EU big-5, JP, KR, AU,
 *      CA, BR, IN, plus the Persian Gulf carriers)
 *   4. EU member states with their own block (each ≥ 32k addresses)
 *
 * Sorting: ascending by `lo`. Binary search assumes non-overlapping
 * (which ICAO 7910 guarantees) ranges.
 *
 * Future expansion: replace this curated subset with the full Doc 7910
 * table (~280 entries), either transcribed from the canonical ICAO PDF
 * or generated from readsb's src/track.c (BSD). Until then, unknown
 * ICAOs render with an empty country column (intentional — no
 * misleading guesses).
 */

#include "icao_country.h"

#include <stddef.h>

typedef struct {
    uint32_t       lo;
    uint32_t       hi;
    pk_country_t   c;
} icao_range_t;

/* Country records — pointer-shared so the binary-search table stays
 * compact. Listed alphabetically by ISO2 for ease of audit. */
static const pk_country_t C_AE = { "AE", "United Arab Emirates" };
static const pk_country_t C_AR = { "AR", "Argentina"            };
static const pk_country_t C_AT = { "AT", "Austria"              };
static const pk_country_t C_AU = { "AU", "Australia"            };
static const pk_country_t C_BE = { "BE", "Belgium"              };
static const pk_country_t C_BD = { "BD", "Bangladesh"           };
static const pk_country_t C_BR = { "BR", "Brazil"               };
static const pk_country_t C_CA = { "CA", "Canada"               };
static const pk_country_t C_CH = { "CH", "Switzerland"          };
static const pk_country_t C_CL = { "CL", "Chile"                };
static const pk_country_t C_CN = { "CN", "China"                };
static const pk_country_t C_CO = { "CO", "Colombia"             };
static const pk_country_t C_CZ = { "CZ", "Czech Republic"       };
static const pk_country_t C_DE = { "DE", "Germany"              };
static const pk_country_t C_DK = { "DK", "Denmark"              };
static const pk_country_t C_EG = { "EG", "Egypt"                };
static const pk_country_t C_ES = { "ES", "Spain"                };
static const pk_country_t C_FI = { "FI", "Finland"              };
static const pk_country_t C_FR = { "FR", "France"               };
static const pk_country_t C_GB = { "GB", "United Kingdom"       };
static const pk_country_t C_GR = { "GR", "Greece"               };
static const pk_country_t C_HK = { "HK", "Hong Kong"            };
static const pk_country_t C_HU = { "HU", "Hungary"              };
static const pk_country_t C_ID = { "ID", "Indonesia"            };
static const pk_country_t C_IE = { "IE", "Ireland"              };
static const pk_country_t C_IL = { "IL", "Israel"               };
static const pk_country_t C_IN = { "IN", "India"                };
static const pk_country_t C_IR = { "IR", "Iran"                 };
static const pk_country_t C_IT = { "IT", "Italy"                };
static const pk_country_t C_JP = { "JP", "Japan"                };
static const pk_country_t C_KH = { "KH", "Cambodia"             };
static const pk_country_t C_KP = { "KP", "North Korea"          };
static const pk_country_t C_KR = { "KR", "South Korea"          };
static const pk_country_t C_KW = { "KW", "Kuwait"               };
static const pk_country_t C_LA = { "LA", "Laos"                 };
static const pk_country_t C_LU = { "LU", "Luxembourg"           };
static const pk_country_t C_MA = { "MA", "Morocco"              };
static const pk_country_t C_MM = { "MM", "Myanmar"              };
static const pk_country_t C_MN = { "MN", "Mongolia"             };
static const pk_country_t C_MO = { "MO", "Macao"                };
static const pk_country_t C_MX = { "MX", "Mexico"               };
static const pk_country_t C_MY = { "MY", "Malaysia"             };
static const pk_country_t C_NG = { "NG", "Nigeria"              };
static const pk_country_t C_NL = { "NL", "Netherlands"          };
static const pk_country_t C_NO = { "NO", "Norway"               };
static const pk_country_t C_NP = { "NP", "Nepal"                };
static const pk_country_t C_NZ = { "NZ", "New Zealand"          };
static const pk_country_t C_PH = { "PH", "Philippines"          };
static const pk_country_t C_PK = { "PK", "Pakistan"             };
static const pk_country_t C_PL = { "PL", "Poland"               };
static const pk_country_t C_PT = { "PT", "Portugal"             };
static const pk_country_t C_QA = { "QA", "Qatar"                };
static const pk_country_t C_RO = { "RO", "Romania"              };
static const pk_country_t C_RU = { "RU", "Russia"               };
static const pk_country_t C_SA = { "SA", "Saudi Arabia"         };
static const pk_country_t C_SE = { "SE", "Sweden"               };
static const pk_country_t C_SG = { "SG", "Singapore"            };
static const pk_country_t C_TH = { "TH", "Thailand"             };
static const pk_country_t C_TN = { "TN", "Tunisia"              };
static const pk_country_t C_TR = { "TR", "Turkey"               };
static const pk_country_t C_TW = { "TW", "Taiwan"               };
static const pk_country_t C_UA = { "UA", "Ukraine"              };
static const pk_country_t C_US = { "US", "United States"        };
static const pk_country_t C_VN = { "VN", "Vietnam"              };
static const pk_country_t C_ZA = { "ZA", "South Africa"         };

/* Sorted by `lo` — keep that property invariant so the binary search
 * works. ICAO Doc 7910 guarantees non-overlapping ranges, so each
 * address is in at most one entry. */
static const icao_range_t s_ranges[] = {
    /* lo,        hi,       country */
    { 0x008000, 0x00FFFF, { C_ZA.iso2, C_ZA.name } },
    { 0x010000, 0x017FFF, { C_EG.iso2, C_EG.name } },
    { 0x020000, 0x027FFF, { C_MA.iso2, C_MA.name } },
    { 0x028000, 0x02FFFF, { C_TN.iso2, C_TN.name } },
    { 0x0A8000, 0x0AFFFF, { C_NG.iso2, C_NG.name } },
    { 0x140000, 0x1BFFFF, { C_RU.iso2, C_RU.name } },  /* large block */
    { 0x300000, 0x33FFFF, { C_IT.iso2, C_IT.name } },
    { 0x340000, 0x37FFFF, { C_ES.iso2, C_ES.name } },
    { 0x380000, 0x3BFFFF, { C_FR.iso2, C_FR.name } },
    { 0x3C0000, 0x3FFFFF, { C_DE.iso2, C_DE.name } },
    { 0x400000, 0x43FFFF, { C_GB.iso2, C_GB.name } },
    { 0x440000, 0x447FFF, { C_AT.iso2, C_AT.name } },
    { 0x448000, 0x44FFFF, { C_BE.iso2, C_BE.name } },
    { 0x458000, 0x45FFFF, { C_DK.iso2, C_DK.name } },
    { 0x460000, 0x467FFF, { C_FI.iso2, C_FI.name } },
    { 0x468000, 0x46FFFF, { C_GR.iso2, C_GR.name } },
    { 0x470000, 0x477FFF, { C_HU.iso2, C_HU.name } },
    { 0x478000, 0x47FFFF, { C_NO.iso2, C_NO.name } },
    { 0x480000, 0x487FFF, { C_NL.iso2, C_NL.name } },
    { 0x488000, 0x48FFFF, { C_PL.iso2, C_PL.name } },
    { 0x490000, 0x497FFF, { C_PT.iso2, C_PT.name } },
    { 0x498000, 0x49FFFF, { C_CZ.iso2, C_CZ.name } },
    { 0x4A0000, 0x4A7FFF, { C_RO.iso2, C_RO.name } },
    { 0x4A8000, 0x4AFFFF, { C_SE.iso2, C_SE.name } },
    { 0x4B0000, 0x4B7FFF, { C_CH.iso2, C_CH.name } },
    { 0x4B8000, 0x4BFFFF, { C_TR.iso2, C_TR.name } },
    { 0x4CA000, 0x4CAFFF, { C_IE.iso2, C_IE.name } },
    { 0x4D0000, 0x4D03FF, { C_LU.iso2, C_LU.name } },
    { 0x506000, 0x506FFF, { C_UA.iso2, C_UA.name } },
    { 0x700000, 0x700FFF, { C_NP.iso2, C_NP.name } },  /* Nepal — small */
    { 0x702000, 0x702FFF, { C_BD.iso2, C_BD.name } },
    { 0x704000, 0x707FFF, { C_MM.iso2, C_MM.name } },
    { 0x708000, 0x70BFFF, { C_KW.iso2, C_KW.name } },
    { 0x70C000, 0x70CFFF, { C_LA.iso2, C_LA.name } },
    { 0x710000, 0x717FFF, { C_SA.iso2, C_SA.name } },
    { 0x718000, 0x71BFFF, { C_KR.iso2, C_KR.name } },
    { 0x71C000, 0x71FFFF, { C_KP.iso2, C_KP.name } },
    { 0x720000, 0x727FFF, { C_MN.iso2, C_MN.name } },
    { 0x728000, 0x72FFFF, { C_PK.iso2, C_PK.name } },  /* approximate */
    { 0x738000, 0x73FFFF, { C_IL.iso2, C_IL.name } },
    { 0x740000, 0x747FFF, { C_IR.iso2, C_IR.name } },
    { 0x750000, 0x757FFF, { C_MY.iso2, C_MY.name } },
    { 0x758000, 0x75BFFF, { C_PH.iso2, C_PH.name } },
    { 0x760000, 0x767FFF, { C_KH.iso2, C_KH.name } },
    { 0x768000, 0x76BFFF, { C_SG.iso2, C_SG.name } },
    { 0x770000, 0x777FFF, { C_VN.iso2, C_VN.name } },
    { 0x780000, 0x7BFFFF, { C_CN.iso2, C_CN.name } },  /* big block */
    { 0x7C0000, 0x7FFFFF, { C_AU.iso2, C_AU.name } },
    { 0x800000, 0x83FFFF, { C_IN.iso2, C_IN.name } },
    { 0x840000, 0x87FFFF, { C_JP.iso2, C_JP.name } },
    { 0x880000, 0x887FFF, { C_TH.iso2, C_TH.name } },
    { 0x888000, 0x88BFFF, { C_ID.iso2, C_ID.name } },  /* approximate */
    { 0x896000, 0x896FFF, { C_AE.iso2, C_AE.name } },
    { 0x899000, 0x8993FF, { C_TW.iso2, C_TW.name } },
    { 0x899400, 0x8997FF, { C_HK.iso2, C_HK.name } },
    { 0x899800, 0x899FFF, { C_MO.iso2, C_MO.name } },
    { 0xA00000, 0xAFFFFF, { C_US.iso2, C_US.name } },  /* huge block */
    { 0xC00000, 0xC3FFFF, { C_CA.iso2, C_CA.name } },
    { 0xC80000, 0xC87FFF, { C_NZ.iso2, C_NZ.name } },
    { 0xE00000, 0xE3FFFF, { C_AR.iso2, C_AR.name } },
    { 0xE40000, 0xE7FFFF, { C_BR.iso2, C_BR.name } },
    { 0xE80000, 0xE80FFF, { C_CL.iso2, C_CL.name } },
    { 0xE94000, 0xE94FFF, { C_CO.iso2, C_CO.name } },
    { 0xE98000, 0xE98FFF, { C_MX.iso2, C_MX.name } },
    { 0xF00000, 0xF07FFF, { C_QA.iso2, C_QA.name } },
};
#define S_RANGES_N (sizeof(s_ranges) / sizeof(s_ranges[0]))

const pk_country_t *pk_country_from_icao24(uint32_t icao24)
{
    /* Binary search: find the largest entry with lo <= icao24, then
     * check that hi >= icao24. Ranges are sorted and non-overlapping. */
    icao24 &= 0xFFFFFF;
    int lo = 0, hi = (int)S_RANGES_N - 1;
    int hit = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (s_ranges[mid].lo <= icao24) {
            hit = mid;
            lo  = mid + 1;
        } else {
            hi  = mid - 1;
        }
    }
    if (hit < 0)                            return NULL;
    if (icao24 > s_ranges[hit].hi)          return NULL;
    return &s_ranges[hit].c;
}
