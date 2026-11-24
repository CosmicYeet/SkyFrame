#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "skyframe_engine.h"

/* Family birthday reminders, from /birthdays.txt on the microSD card (format: see
   sf_birthday_parse_line) with photos in /birthdays/. The line marked "owner" is the
   person this SkyFrame belongs to: their birthday gets the special screen, with the
   message from the file's "message: ..." line. Nothing about the family is built in,
   so another SkyFrame only needs a different birthdays.txt. */
#define DEFAULT_OWNER_MESSAGE "With love from your family"

#define BIRTHDAY_PHOTO_SIZE 180     /* square RGB565 photo, shown in a circle */
#define BIRTHDAYS_MAX 6

typedef struct {
    sf_birthday_t who;
    int age;                        /* -1 when the birth year is unknown */
    uint16_t *pixels;               /* BIRTHDAY_PHOTO_SIZE^2 RGB565, or NULL */
} birthday_today_t;
typedef struct {
    int count;
    bool preview;                   /* test mode: never saved when dismissed */
    char message[64];               /* owner's birthday message */
    birthday_today_t list[BIRTHDAYS_MAX];
} birthday_list_t;

/* Start the background loader (CPU1). */
void birthdays_start(void);
/* Tell the loader today's local date (call whenever the clock is known). */
void birthdays_set_today(int year, int month, int day);
/* Non-blocking: true when a new list for today is ready. The caller takes ownership
   of the photo buffers in *out and frees them (free()) when done. */
bool birthdays_take(birthday_list_t *out);
/* Hide this person's reminder until next year (remembered across restarts). */
void birthdays_dismiss(const sf_birthday_t *who, int year);
/* Test mode: deliver a preview list (the owner's birthday, or the next family
   birthday from the file; a sample if there is none) through birthdays_take(),
   ignoring dismissals. */
void birthdays_preview(bool owner);
/* Test mode: the index-th upcoming family birthday (0 = the next one), wrapping around. */
void birthdays_preview_family(int index);
/* Rebuild today's real list (e.g. after a preview). */
void birthdays_refresh(void);
