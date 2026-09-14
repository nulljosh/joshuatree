#ifndef JSON_H
#define JSON_H
/* Not a JSON parser, a string-value grabber: finds "key":"...", copies the
   value out with \" \\ \n \t \r unescaped, stops at the closing quote.
   Enough to read one field out of an LLM API's JSON response, nothing
   more, no numbers/arrays/nesting awareness. */
unsigned int json_extract_string(const char *json, const char *key, char *out, unsigned int maxlen);

/* v71: the numeric sibling. Finds "key": followed by a bare JSON number
   and copies its exact text ("49.0983", "-122.6498") into out, untouched,
   no float math in this freestanding build. That text is what a URL query
   string wants anyway (weather_fetch splices it straight into Open-Meteo's
   latitude=/longitude= parameters). Returns the number of bytes copied, 0
   if the key is missing or its value isn't a number. Matches the closing
   quote too, so "lat" never matches inside "latitude". */
unsigned int json_extract_number_text(const char *json, const char *key, char *out, unsigned int maxlen);

/* Writes s into out with " \ and control characters escaped, for building
   a JSON string value to send. Returns the number of bytes written. */
unsigned int json_escape(const char *s, char *out, unsigned int maxlen);
#endif
