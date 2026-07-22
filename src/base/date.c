#include "date.h"

static int decimal_pair(const char *data) {
  if (data[0] < '0' || data[0] > '9' || data[1] < '0' || data[1] > '9')
    return -1;
  return (data[0] - '0') * 10 + data[1] - '0';
}

static int leap_year(int year) {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int ab_iso_date_valid(const AbString *value) {
  static const int month_days[] = {31, 28, 31, 30, 31, 30,
                                   31, 31, 30, 31, 30, 31};
  int year;
  int month;
  int day;
  int limit;
  if (!value || value->length != 10 || value->data[4] != '-' ||
      value->data[7] != '-')
    return 0;
  if (value->data[0] < '0' || value->data[0] > '9' || value->data[1] < '0' ||
      value->data[1] > '9' || value->data[2] < '0' || value->data[2] > '9' ||
      value->data[3] < '0' || value->data[3] > '9')
    return 0;
  year = (value->data[0] - '0') * 1000 + (value->data[1] - '0') * 100 +
         (value->data[2] - '0') * 10 + value->data[3] - '0';
  month = decimal_pair(value->data + 5);
  day = decimal_pair(value->data + 8);
  if (year < 1 || month < 1 || month > 12 || day < 1)
    return 0;
  limit = month_days[month - 1] + (month == 2 && leap_year(year));
  return day <= limit;
}

int ab_iso_date_compare(const AbString *left, const AbString *right) {
  return ab_string_compare(left, right);
}
