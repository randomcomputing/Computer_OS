#include "rtc.h"
#include "io.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

// Time registers.
#define CMOS_SECOND 0x00
#define CMOS_MINUTE 0x02
#define CMOS_HOUR   0x04
#define CMOS_DAY    0x07
#define CMOS_MONTH  0x08
#define CMOS_YEAR   0x09
#define CMOS_STATUS_A 0x0A   // bit 7 = update-in-progress
#define CMOS_STATUS_B 0x0B   // bit 1 = 24-hour, bit 2 = binary

// Scratch registers we use to persist the timezone offset. The CMOS
// chip has 50+ bytes of unused space starting around 0x32; the BIOS
// does not touch 0x50..0x7F on any board I know of.
#define CMOS_TZ_LO    0x50   // signed 16-bit offset, low byte
#define CMOS_TZ_HI    0x51   // signed 16-bit offset, high byte
#define CMOS_TZ_MAGIC 0x52   // sentinel — non-magic value means "never set"
#define TZ_MAGIC_BYTE 0x5A   // arbitrary, just must be unlikely-to-be-zero

// --- low-level CMOS port access ---------------------------------------

static unsigned char cmos_read(unsigned char reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static void cmos_write(unsigned char reg, unsigned char val) {
    outb(CMOS_ADDR, reg);
    outb(CMOS_DATA, val);
}

// True if the RTC is in the middle of updating. Reading time registers
// during an update can return inconsistent values.
static int rtc_update_in_progress(void) {
    return cmos_read(CMOS_STATUS_A) & 0x80;
}

static unsigned char bcd_to_bin(unsigned char v) {
    return (v & 0x0F) + ((v >> 4) * 10);
}

// --- raw RTC read -----------------------------------------------------

void rtc_read(rtc_time_t* out) {
    while (rtc_update_in_progress()) { }

    unsigned char sec, min, hr, day, mon;
    unsigned int  yr;
    unsigned char sec2, min2, hr2, day2, mon2;
    unsigned int  yr2;

    do {
        sec = cmos_read(CMOS_SECOND);
        min = cmos_read(CMOS_MINUTE);
        hr  = cmos_read(CMOS_HOUR);
        day = cmos_read(CMOS_DAY);
        mon = cmos_read(CMOS_MONTH);
        yr  = cmos_read(CMOS_YEAR);

        while (rtc_update_in_progress()) { }

        sec2 = cmos_read(CMOS_SECOND);
        min2 = cmos_read(CMOS_MINUTE);
        hr2  = cmos_read(CMOS_HOUR);
        day2 = cmos_read(CMOS_DAY);
        mon2 = cmos_read(CMOS_MONTH);
        yr2  = cmos_read(CMOS_YEAR);
    } while (sec != sec2 || min != min2 || hr != hr2 ||
             day != day2 || mon != mon2 || yr != yr2);

    unsigned char status_b = cmos_read(CMOS_STATUS_B);
    int is_binary = status_b & 0x04;
    int is_24hour = status_b & 0x02;
    int hour_pm = !is_24hour && (hr & 0x80);

    if (!is_binary) {
        sec = bcd_to_bin(sec);
        min = bcd_to_bin(min);
        hr  = bcd_to_bin(hr & 0x7F);
        day = bcd_to_bin(day);
        mon = bcd_to_bin(mon);
        yr  = bcd_to_bin((unsigned char)yr);
    } else if (!is_24hour) {
        hr &= 0x7F;
    }

    if (!is_24hour) {
        if (hr == 12) hr = 0;
        if (hour_pm)  hr += 12;
    }

    unsigned int full_year = (yr <= 70) ? (2000 + yr) : (1900 + yr);

    out->year   = full_year;
    out->month  = mon;
    out->day    = day;
    out->hour   = hr;
    out->minute = min;
    out->second = sec;
}

// --- calendar arithmetic ---------------------------------------------
//
// To apply a timezone offset cleanly we convert (year, month, day) to
// a Julian Day Number, do integer arithmetic in days+minutes, then
// convert back. JDN handles leap years and month rollovers correctly
// without fiddly conditionals.

static int days_to_jdn(int y, int m, int d) {
    // Fliegel & Van Flandern formula (Communications of the ACM, 1968).
    // Handles all dates in the Gregorian calendar past 1 March 4801 BC.
    int a = (14 - m) / 12;
    int yy = y + 4800 - a;
    int mm = m + 12 * a - 3;
    return d + (153 * mm + 2) / 5 + 365 * yy + yy / 4 - yy / 100 + yy / 400 - 32045;
}

static void jdn_to_ymd(int jdn, int* y, int* m, int* d) {
    int a = jdn + 32044;
    int b = (4 * a + 3) / 146097;
    int c = a - (146097 * b) / 4;
    int dd = (4 * c + 3) / 1461;
    int e = c - (1461 * dd) / 4;
    int mm = (5 * e + 2) / 153;
    *d = e - (153 * mm + 2) / 5 + 1;
    *m = mm + 3 - 12 * (mm / 10);
    *y = 100 * b + dd - 4800 + (mm / 10);
}

// Add `offset_minutes` to a time and renormalize all fields. Handles
// rollover/rollback across days, months, years.
static void rtc_apply_offset(rtc_time_t* t, int offset_minutes) {
    if (offset_minutes == 0) return;

    // Total minutes from midnight, signed.
    int total = (int)t->hour * 60 + (int)t->minute + offset_minutes;

    // Days carried forward (or backward) — tricky because C division
    // truncates toward zero, so for negative numerators we need to
    // adjust manually to get floor division.
    int day_delta = total / (24 * 60);
    int new_min   = total % (24 * 60);
    if (new_min < 0) {
        new_min += 24 * 60;
        day_delta -= 1;
    }

    int new_hour = new_min / 60;
    int new_minute = new_min % 60;

    int jdn = days_to_jdn((int)t->year, (int)t->month, (int)t->day);
    jdn += day_delta;

    int y, m, d;
    jdn_to_ymd(jdn, &y, &m, &d);

    t->year   = (unsigned int)y;
    t->month  = (unsigned char)m;
    t->day    = (unsigned char)d;
    t->hour   = (unsigned char)new_hour;
    t->minute = (unsigned char)new_minute;
    // seconds unchanged
}

void rtc_read_local(rtc_time_t* out) {
    rtc_read(out);
    rtc_apply_offset(out, rtc_tz_offset_minutes());
}

// --- timezone offset persistence -------------------------------------

int rtc_tz_offset_minutes(void) {
    if (cmos_read(CMOS_TZ_MAGIC) != TZ_MAGIC_BYTE) {
        return 0;   // never set → UTC
    }
    unsigned int lo = cmos_read(CMOS_TZ_LO);
    unsigned int hi = cmos_read(CMOS_TZ_HI);
    unsigned int u = (hi << 8) | lo;
    // Sign-extend the 16-bit value to int.
    int v = (int)u;
    if (v & 0x8000) v -= 0x10000;
    return v;
}

void rtc_tz_set_offset_minutes(int minutes) {
    // Clamp to a sane signed-16 range. Real-world offsets fit in
    // [-720, +840] minutes (UTC-12 to UTC+14), well inside int16.
    if (minutes < -720) minutes = -720;
    if (minutes >  840) minutes =  840;

    unsigned int u = (unsigned int)(minutes & 0xFFFF);
    cmos_write(CMOS_TZ_LO,    (unsigned char)(u & 0xFF));
    cmos_write(CMOS_TZ_HI,    (unsigned char)((u >> 8) & 0xFF));
    cmos_write(CMOS_TZ_MAGIC, TZ_MAGIC_BYTE);
}

// --- named-zone table ------------------------------------------------
//
// Common abbreviations + all the non-DST whole-hour offsets so most
// places have a usable name. Half-hour and 45-minute zones are
// represented by their abbreviations where they exist.
//
// First match for a given offset is the "canonical" name returned by
// rtc_tz_name_for_offset — order matters here.

static const struct {
    const char* name;
    int         minutes;
} zones[] = {
    // UTC + Atlantic (canonical first)
    { "UTC",  0       },
    { "GMT",  0       },
    { "Z",    0       },

    // North America
    { "EST", -5 * 60  },
    { "EDT", -4 * 60  },
    { "CST", -6 * 60  },
    { "CDT", -5 * 60  },   // duplicate offset — EST printed as canonical
    { "MST", -7 * 60  },
    { "MDT", -6 * 60  },   // duplicate offset
    { "PST", -8 * 60  },
    { "PDT", -7 * 60  },   // duplicate offset
    { "AKST", -9 * 60 },
    { "HST", -10 * 60 },

    // Europe
    { "BST", +1 * 60  },   // British Summer Time / also Bangladesh — eh
    { "CET", +1 * 60  },
    { "CEST", +2 * 60 },
    { "EET", +2 * 60  },
    { "EEST", +3 * 60 },

    // Asia / Pacific
    { "MSK", +3 * 60  },
    { "IST",  5 * 60 + 30 },   // India
    { "PKT", +5 * 60  },
    { "CST_CN", +8 * 60 },     // China — disambiguated to avoid US clash
    { "JST", +9 * 60  },
    { "KST", +9 * 60  },
    { "AEST", +10 * 60 },
    { "AEDT", +11 * 60 },
    { "NZST", +12 * 60 },
    { "NZDT", +13 * 60 },
};

#define ZONE_COUNT ((int)(sizeof(zones) / sizeof(zones[0])))

// Case-insensitive compare.
static int ieq(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

int rtc_tz_lookup(const char* name, int* out_minutes) {
    for (int i = 0; i < ZONE_COUNT; i++) {
        if (ieq(name, zones[i].name)) {
            if (out_minutes) *out_minutes = zones[i].minutes;
            return 1;
        }
    }
    return 0;
}

int rtc_tz_iter(int i, const char** out_name, int* out_minutes) {
    if (i < 0 || i >= ZONE_COUNT) return 0;
    if (out_name)    *out_name    = zones[i].name;
    if (out_minutes) *out_minutes = zones[i].minutes;
    return 1;
}

const char* rtc_tz_name_for_offset(int minutes) {
    for (int i = 0; i < ZONE_COUNT; i++) {
        if (zones[i].minutes == minutes) return zones[i].name;
    }
    return 0;
}