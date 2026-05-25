/*
 * airline_codes.h — ICAO 3-letter airline code → IATA + operator name.
 *
 * The callsign field in DF17 metype 1-4 carries the ICAO format
 * (e.g. "CSN1234" = China Southern flight 1234). Most pilots and
 * passengers recognise the IATA form ("CZ1234") instead. This module
 * provides the static lookup CSN → ("CZ", "China Southern Airlines").
 *
 * Generated from Wikipedia's per-letter airline code pages, with a
 * small maintainer-reviewed MANUAL_ADDITIONS list in the generator for
 * carriers missing from those pages.
 *
 * Not exhaustive: unknown ICAO codes return NULL and the renderer
 * falls back to the raw ICAO callsign.
 *
 * Lookup: binary search over the sorted generated table, currently
 * about 5500 entries. Refresh with firmware/scripts/gen_airline_codes.py.
 */
#pragma once

typedef struct {
    const char *icao3;    /* 3-letter ICAO code, e.g. "CSN" */
    const char *iata2;    /* 2-letter IATA code, e.g. "CZ"; NULL if no IATA assigned */
    const char *name;     /* Operator full name */
} pk_airline_t;

/*
 * Look up an airline by its 3-letter ICAO code (case-sensitive,
 * uppercase). Returns NULL when not in the table. The pointer is
 * valid for the program lifetime (static data).
 */
const pk_airline_t *pk_airline_from_icao3(const char *icao3);

/*
 * Extract the 3-letter airline prefix from a raw ADS-B callsign and
 * look it up. The callsign is the form "AAA####" where the first 3
 * chars are letters and the rest digits — this helper handles the
 * parse, validates the shape, and returns NULL if the callsign isn't
 * in that form (e.g. tail-number registrations like "N12345" or
 * military "RCH123"). Out parameter `flight_number_out` is filled with
 * a pointer into `callsign` past the 3-letter prefix (so the caller
 * can compose "CZ" + "1234" → "CZ1234").
 */
const pk_airline_t *pk_airline_from_callsign(const char *callsign,
                                             const char **flight_number_out);
