#ifndef IMG_H
#define IMG_H
/* Images through ImageMagick (`magick`); inline display over the kitty
 * graphics protocol (Ghostty), else the path is printed. */
int img_is_image_path(const char *path);                  /* jpg png gif webp bmp */
int img_size(const char *path, int *w, int *h);           /* 0 ok */
int img_to_png(const char *src, const char *dst, int max); /* rasterize/convert (SVG too), fit max px; 0 ok */
void img_show(const char *path, const char *caption);      /* caller holds term_mu */
/* Typeset display math with pdflatex and show it inline; -1 (nothing printed)
 * without kitty graphics, pdflatex or poppler, or when the TeX fails. */
int img_show_tex(const char *tex);
#endif
