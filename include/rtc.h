#ifndef RTC_H
#define RTC_H

// CMOS Real-Time Clock + timezone offset.
//
// The RTC chip keeps wall-clock time across reboots, powered by the
// motherboard battery. It's accessed through I/O ports 0x70/0x71:
//   - write the register number to 0x70
//   - read or write the value at 0x71
//
// We also use three of the chip's unused scratch registers (0x50..0x52)
// to persist a UTC offset across reboots. This is "user CMOS RAM" that
// the BIOS does not touch.

typedef struct {
    unsigned int  year;     // full year (e.g. 2026)
    unsigned char month;    // 1..12
    unsigned char day;      // 1..31
    unsigned char hour;     // 0..23
    unsigned char minute;   // 0..59
    unsigned char second;   // 0..59
} rtc_time_t;

// Read the current wall-clock time directly from the RTC, with no
// timezone adjustment. Always succeeds. Values are normalized to
// 24-hour binary regardless of the chip's BCD/12-hour mode.
void rtc_read(rtc_time_t* out);

// Read the current time and apply the saved UTC offset. This is what
// the `date` command prints. If no offset has been set, behaves
// identically to rtc_read.
void rtc_read_local(rtc_time_t* out);

// --- timezone API -------------------------------------------------

// Get the saved UTC offset in minutes (negative = west of UTC). Returns
// 0 if no offset has been persisted (treated as UTC).
int rtc_tz_offset_minutes(void);

// Set and persist a UTC offset. Survives reboots via CMOS scratch RAM.
void rtc_tz_set_offset_minutes(int minutes);

// Look up a zone name. Returns 1 on success and writes the offset (in
// minutes) into *out_minutes. Returns 0 if the name isn't known.
// Names are case-insensitive ("UTC", "utc", "Utc" all work).
int rtc_tz_lookup(const char* name, int* out_minutes);

// Walk the named-zone table. i starts at 0; returns 0 when there are
// no more entries. *out_name and *out_minutes are written when the
// entry exists. Used by the `tz` command to print the available list.
int rtc_tz_iter(int i, const char** out_name, int* out_minutes);

// Best-effort reverse lookup: find a zone name matching the given
// offset. Returns NULL if none does. Multiple zones can share an
// offset (e.g. EST and CDT are both UTC-5); we return the first match
// in table order.
const char* rtc_tz_name_for_offset(int minutes);

#endif