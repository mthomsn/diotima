#ifndef CHROME_H
#define CHROME_H
/* NULL when headless Chrome is usable, else why not. */
const char *chrome_unavailable(void);
/* Load html_path (absolute) headlessly, let it settle ~2s, evaluate probe (optional JS).
 * Returns a malloc'd "PASSED: …" / "FAILED: …" report; *passed set. */
char *chrome_test(const char *html_path, const char *probe, int *passed);
#endif
