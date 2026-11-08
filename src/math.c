#include "math.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int (*math_display)(const char *tex);

/* ── growable string ───────────────────────────────────────────────────── */

typedef struct { char *s; size_t n, cap; } sb_t;

static void sb_add(sb_t *b, const char *s, size_t n) {
	if (b->n + n + 1 > b->cap) {
		b->cap = (b->n + n + 1) * 2;
		b->s = realloc(b->s, b->cap);
	}
	memcpy(b->s + b->n, s, n);
	b->n += n;
	b->s[b->n] = 0;
}
static void sb_put(sb_t *b, const char *s) { sb_add(b, s, strlen(s)); }
static char *sb_take(sb_t *b) { if (!b->s) sb_add(b, "", 0); return b->s; }

static size_t u8len(const char *s) {
	unsigned char c = (unsigned char)*s;
	size_t l = c < 0x80 ? 1 : c >> 5 == 6 ? 2 : c >> 4 == 14 ? 3 : c >> 3 == 30 ? 4 : 1;
	for (size_t i = 1; i < l; i++) if (!s[i]) return i;
	return l;
}

/* ── tables ────────────────────────────────────────────────────────────── */

static const char *const SYM[][2] = {
	{ "alpha", "α" }, { "beta", "β" }, { "gamma", "γ" }, { "delta", "δ" }, { "epsilon", "ϵ" }, { "varepsilon", "ε" },
	{ "zeta", "ζ" }, { "eta", "η" }, { "theta", "θ" }, { "vartheta", "ϑ" }, { "iota", "ι" }, { "kappa", "κ" },
	{ "lambda", "λ" }, { "mu", "μ" }, { "nu", "ν" }, { "xi", "ξ" }, { "pi", "π" }, { "varpi", "ϖ" }, { "rho", "ρ" },
	{ "varrho", "ϱ" }, { "sigma", "σ" }, { "varsigma", "ς" }, { "tau", "τ" }, { "upsilon", "υ" }, { "phi", "ϕ" },
	{ "varphi", "φ" }, { "chi", "χ" }, { "psi", "ψ" }, { "omega", "ω" },
	{ "Gamma", "Γ" }, { "Delta", "Δ" }, { "Theta", "Θ" }, { "Lambda", "Λ" }, { "Xi", "Ξ" }, { "Pi", "Π" },
	{ "Sigma", "Σ" }, { "Upsilon", "Υ" }, { "Phi", "Φ" }, { "Psi", "Ψ" }, { "Omega", "Ω" },
	{ "infty", "∞" }, { "sum", "∑" }, { "prod", "∏" }, { "coprod", "∐" }, { "int", "∫" }, { "iint", "∬" },
	{ "iiint", "∭" }, { "oint", "∮" }, { "partial", "∂" }, { "nabla", "∇" }, { "cdot", "·" }, { "times", "×" },
	{ "div", "÷" }, { "pm", "±" }, { "mp", "∓" }, { "leq", "≤" }, { "le", "≤" }, { "geq", "≥" }, { "ge", "≥" },
	{ "leqslant", "⩽" }, { "geqslant", "⩾" }, { "neq", "≠" }, { "ne", "≠" }, { "approx", "≈" }, { "equiv", "≡" },
	{ "sim", "∼" }, { "simeq", "≃" }, { "cong", "≅" }, { "propto", "∝" }, { "ll", "≪" }, { "gg", "≫" },
	{ "to", "→" }, { "rightarrow", "→" }, { "leftarrow", "←" }, { "gets", "←" }, { "leftrightarrow", "↔" },
	{ "Rightarrow", "⇒" }, { "Leftarrow", "⇐" }, { "Leftrightarrow", "⇔" }, { "iff", "⟺" }, { "implies", "⟹" },
	{ "impliedby", "⟸" }, { "mapsto", "↦" }, { "longrightarrow", "⟶" }, { "longmapsto", "⟼" }, { "uparrow", "↑" },
	{ "downarrow", "↓" }, { "hookrightarrow", "↪" }, { "rightharpoonup", "⇀" },
	{ "in", "∈" }, { "notin", "∉" }, { "ni", "∋" }, { "subset", "⊂" }, { "subseteq", "⊆" }, { "subsetneq", "⊊" },
	{ "supset", "⊃" }, { "supseteq", "⊇" }, { "cup", "∪" }, { "cap", "∩" }, { "bigcup", "⋃" }, { "bigcap", "⋂" },
	{ "setminus", "∖" }, { "emptyset", "∅" }, { "varnothing", "∅" }, { "forall", "∀" }, { "exists", "∃" },
	{ "nexists", "∄" }, { "neg", "¬" }, { "lnot", "¬" }, { "land", "∧" }, { "wedge", "∧" }, { "lor", "∨" },
	{ "vee", "∨" }, { "bigwedge", "⋀" }, { "bigvee", "⋁" }, { "oplus", "⊕" }, { "otimes", "⊗" }, { "odot", "⊙" },
	{ "bigoplus", "⨁" }, { "bigotimes", "⨂" }, { "ldots", "…" }, { "dots", "…" }, { "cdots", "⋯" }, { "vdots", "⋮" },
	{ "ddots", "⋱" }, { "circ", "∘" }, { "bullet", "•" }, { "star", "⋆" }, { "ast", "∗" }, { "langle", "⟨" },
	{ "rangle", "⟩" }, { "lfloor", "⌊" }, { "rfloor", "⌋" }, { "lceil", "⌈" }, { "rceil", "⌉" }, { "mid", "∣" },
	{ "nmid", "∤" }, { "parallel", "∥" }, { "perp", "⊥" }, { "angle", "∠" }, { "triangle", "△" }, { "square", "□" },
	{ "degree", "°" }, { "prime", "′" }, { "hbar", "ℏ" }, { "ell", "ℓ" }, { "Re", "ℜ" }, { "Im", "ℑ" },
	{ "aleph", "ℵ" }, { "top", "⊤" }, { "bot", "⊥" }, { "vdash", "⊢" }, { "models", "⊨" }, { "vert", "|" },
	{ "Vert", "‖" }, { "lvert", "|" }, { "rvert", "|" }, { "lVert", "‖" }, { "rVert", "‖" }, { "colon", ":" },
	{ "cdotp", "·" }, { "therefore", "∴" }, { "because", "∵" }, { "checkmark", "✓" }, { "dagger", "†" },
	{ "lbrace", "{" }, { "rbrace", "}" }, { "backslash", "\\" }, { "prec", "≺" }, { "succ", "≻" },
	{ "preceq", "⪯" }, { "succeq", "⪰" }, { "quad", "  " }, { "qquad", "    " },
	{ "lt", "<" }, { "gt", ">" }, { "S", "§" }, { "wp", "℘" }, { "Box", "□" }, { "diamond", "⋄" },
};

/* Commands that only steer typesetting: dropped. */
static const char *const NOP[] = {
	"left", "right", "big", "Big", "bigg", "Bigg", "bigl", "bigr", "Bigl", "Bigr", "biggl", "biggr", "middle",
	"displaystyle", "textstyle", "scriptstyle", "limits", "nolimits", "nonumber", "notag", "noindent", NULL,
};

/* Commands whose one argument prints as itself. */
static const char *const WRAP[] = {
	"text", "textrm", "textit", "textbf", "textsf", "texttt", "mathrm", "mathit", "mathbf", "mathsf", "mathtt",
	"mathcal", "mathscr", "mathfrak", "operatorname", "mbox", "boldsymbol", "bm", "emph", "overbrace", "underbrace",
	"mathop", "mathrel", "mathbin", "boxed", "pmb", "hbox", NULL,
};

static const char *const ACCENT[][2] = {
	{ "hat", "\xcc\x82" }, { "widehat", "\xcc\x82" }, { "bar", "\xcc\x85" }, { "overline", "\xcc\x85" },
	{ "vec", "\xe2\x83\x97" }, { "dot", "\xcc\x87" }, { "ddot", "\xcc\x88" }, { "tilde", "\xcc\x83" },
	{ "widetilde", "\xcc\x83" }, { "underline", "\xcc\xb2" }, { "check", "\xcc\x8c" }, { "acute", "\xcc\x81" },
	{ "grave", "\xcc\x80" }, { "breve", "\xcc\x86" },
};

static const char *const BB[][2] = {
	{ "R", "ℝ" }, { "N", "ℕ" }, { "Z", "ℤ" }, { "Q", "ℚ" }, { "C", "ℂ" }, { "P", "ℙ" }, { "H", "ℍ" },
	{ "E", "𝔼" }, { "F", "𝔽" }, { "1", "𝟙" },
};

static const char *const SUP[][2] = {
	{ "0", "⁰" }, { "1", "¹" }, { "2", "²" }, { "3", "³" }, { "4", "⁴" }, { "5", "⁵" }, { "6", "⁶" }, { "7", "⁷" },
	{ "8", "⁸" }, { "9", "⁹" }, { "+", "⁺" }, { "-", "⁻" }, { "−", "⁻" }, { "=", "⁼" }, { "(", "⁽" }, { ")", "⁾" },
	{ "a", "ᵃ" }, { "b", "ᵇ" }, { "c", "ᶜ" }, { "d", "ᵈ" }, { "e", "ᵉ" }, { "f", "ᶠ" }, { "g", "ᵍ" }, { "h", "ʰ" },
	{ "i", "ⁱ" }, { "j", "ʲ" }, { "k", "ᵏ" }, { "l", "ˡ" }, { "m", "ᵐ" }, { "n", "ⁿ" }, { "o", "ᵒ" }, { "p", "ᵖ" },
	{ "r", "ʳ" }, { "s", "ˢ" }, { "t", "ᵗ" }, { "u", "ᵘ" }, { "v", "ᵛ" }, { "w", "ʷ" }, { "x", "ˣ" }, { "y", "ʸ" },
	{ "z", "ᶻ" }, { "A", "ᴬ" }, { "B", "ᴮ" }, { "D", "ᴰ" }, { "E", "ᴱ" }, { "G", "ᴳ" }, { "H", "ᴴ" }, { "I", "ᴵ" },
	{ "J", "ᴶ" }, { "K", "ᴷ" }, { "L", "ᴸ" }, { "M", "ᴹ" }, { "N", "ᴺ" }, { "O", "ᴼ" }, { "P", "ᴾ" }, { "R", "ᴿ" },
	{ "T", "ᵀ" }, { "U", "ᵁ" }, { "V", "ⱽ" }, { "W", "ᵂ" }, { "β", "ᵝ" }, { "γ", "ᵞ" }, { "δ", "ᵟ" }, { "θ", "ᶿ" },
	{ "ϕ", "ᵠ" }, { "φ", "ᵠ" }, { "χ", "ᵡ" }, { "∘", "°" }, { "⊤", "ᵀ" }, { "*", "*" }, { "∗", "*" }, { "′", "′" },
	{ "†", "†" },
};

static const char *const SUB[][2] = {
	{ "0", "₀" }, { "1", "₁" }, { "2", "₂" }, { "3", "₃" }, { "4", "₄" }, { "5", "₅" }, { "6", "₆" }, { "7", "₇" },
	{ "8", "₈" }, { "9", "₉" }, { "+", "₊" }, { "-", "₋" }, { "−", "₋" }, { "=", "₌" }, { "(", "₍" }, { ")", "₎" },
	{ "a", "ₐ" }, { "e", "ₑ" }, { "h", "ₕ" }, { "i", "ᵢ" }, { "j", "ⱼ" }, { "k", "ₖ" }, { "l", "ₗ" }, { "m", "ₘ" },
	{ "n", "ₙ" }, { "o", "ₒ" }, { "p", "ₚ" }, { "r", "ᵣ" }, { "s", "ₛ" }, { "t", "ₜ" }, { "u", "ᵤ" }, { "v", "ᵥ" },
	{ "x", "ₓ" }, { "β", "ᵦ" }, { "γ", "ᵧ" }, { "ρ", "ᵨ" }, { "ϕ", "ᵩ" }, { "φ", "ᵩ" }, { "χ", "ᵪ" },
};

#define LOOKUP(tab, key, len) lookup(tab, sizeof tab / sizeof *tab, key, len)
static const char *lookup(const char *const tab[][2], size_t n, const char *k, size_t l) {
	for (size_t i = 0; i < n; i++) if (strlen(tab[i][0]) == l && !memcmp(tab[i][0], k, l)) return tab[i][1];
	return NULL;
}
static int in_list(const char *const *list, const char *k, size_t l) {
	for (; *list; list++) if (strlen(*list) == l && !memcmp(*list, k, l)) return 1;
	return 0;
}

/* ── converter ─────────────────────────────────────────────────────────── */

typedef struct { const char *p; } P;

static void seq(P *p, sb_t *o, char stop);
static void cmd(P *p, sb_t *o);

static void skip_sp(P *p) { while (*p->p == ' ' || *p->p == '\t' || *p->p == '\n') p->p++; }

/* One argument: {group}, a command, or a single character. */
static void arg(P *p, sb_t *o) {
	skip_sp(p);
	if (!*p->p) return;
	if (*p->p == '{') { p->p++; seq(p, o, '}'); return; }
	const char *s = p->p;
	if (*s == '\\') { cmd(p, o); return; } /* \frac in x^\frac12 reads its own arguments */
	size_t l = u8len(s);
	sb_add(o, s, l);
	p->p += l;
}

static size_t cps(const char *s) { size_t n = 0; for (; *s; s += u8len(s)) n++; return n; }

static int all_alnum(const char *s) {
	for (; *s; s += u8len(s)) if ((unsigned char)*s < 0x80 && !isalnum((unsigned char)*s)) return 0;
	return 1;
}

/* x^{…} / x_{…}: real super/subscript glyphs when every character has one. */
static void script(sb_t *o, const char *s, int sup) {
	sb_t m = { 0 };
	int ok = 1;
	for (const char *c = s; *c && ok; c += u8len(c)) {
		if (*c == ' ') continue;
		const char *g = sup ? LOOKUP(SUP, c, u8len(c)) : LOOKUP(SUB, c, u8len(c));
		if (g) sb_put(&m, g); else ok = 0;
	}
	if (ok && m.n) sb_add(o, m.s, m.n);
	else if (*s) {
		sb_put(o, sup ? "^" : "_");
		if (cps(s) > 1) { sb_put(o, "("); sb_put(o, s); sb_put(o, ")"); } else sb_put(o, s);
	}
	free(m.s);
}

static void paren(sb_t *o, const char *s) {
	if (cps(s) > 1 && !all_alnum(s)) { sb_put(o, "("); sb_put(o, s); sb_put(o, ")"); } else sb_put(o, s);
}

static void cmd(P *p, sb_t *o) {
	const char *s = ++p->p;
	if (!isalpha((unsigned char)*s)) { /* control symbol */
		if (!*s) return;
		p->p++;
		switch (*s) {
		case ',': case ':': case '>': case ' ': sb_put(o, " "); return;
		case ';': sb_put(o, " "); return;
		case '!': return;
		case '\\': sb_put(o, "\n"); return;
		case '|': sb_put(o, "‖"); return;
		default: sb_add(o, s, 1); return; /* \{ \} \% \_ \& \# \$ */
		}
	}
	while (isalpha((unsigned char)*p->p)) p->p++;
	size_t l = (size_t)(p->p - s);
	const char *g;
#define IS(w) (l == sizeof w - 1 && !memcmp(s, w, l))
	if ((g = LOOKUP(SYM, s, l))) { sb_put(o, g); return; }
	if (in_list(NOP, s, l)) {
		if (IS("left") || IS("right")) { skip_sp(p); if (*p->p == '.') p->p++; }
		return;
	}
	if (in_list(WRAP, s, l)) { arg(p, o); return; }
	if ((g = LOOKUP(ACCENT, s, l))) { arg(p, o); sb_put(o, g); return; }
	if (IS("frac") || IS("dfrac") || IS("tfrac") || IS("cfrac")) {
		sb_t a = { 0 }, b = { 0 };
		arg(p, &a); arg(p, &b);
		paren(o, sb_take(&a)); sb_put(o, "/"); paren(o, sb_take(&b));
		free(a.s); free(b.s);
		return;
	}
	if (IS("binom") || IS("dbinom") || IS("tbinom")) {
		sb_t a = { 0 }, b = { 0 };
		arg(p, &a); arg(p, &b);
		sb_put(o, "C("); sb_put(o, sb_take(&a)); sb_put(o, ","); sb_put(o, sb_take(&b)); sb_put(o, ")");
		free(a.s); free(b.s);
		return;
	}
	if (IS("sqrt")) {
		skip_sp(p);
		if (*p->p == '[') {
			sb_t n = { 0 };
			p->p++;
			seq(p, &n, ']');
			script(o, sb_take(&n), 1);
			free(n.s);
		}
		sb_t a = { 0 };
		arg(p, &a);
		sb_put(o, "√");
		paren(o, sb_take(&a));
		free(a.s);
		return;
	}
	if (IS("mathbb") || IS("mathds") || IS("bbold")) {
		sb_t a = { 0 };
		arg(p, &a);
		for (const char *c = sb_take(&a); *c; c += u8len(c)) {
			const char *d = LOOKUP(BB, c, u8len(c));
			if (d) sb_put(o, d); else sb_add(o, c, u8len(c));
		}
		free(a.s);
		return;
	}
	if (IS("not")) {
		skip_sp(p);
		if (*p->p == '=') { p->p++; sb_put(o, "≠"); return; }
		arg(p, o);
		sb_put(o, "\xcc\xb8");
		return;
	}
	if (IS("begin") || IS("end")) {
		sb_t e = { 0 };
		arg(p, &e);
		const char *en = sb_take(&e);
		if (IS("begin") && (!strncmp(en, "array", 5) || !strncmp(en, "tabular", 7))) { sb_t x = { 0 }; arg(p, &x); free(x.s); }
		if (strstr(en, "matrix") || !strncmp(en, "cases", 5)) sb_put(o, IS("begin") ? (strstr(en, "pmatrix") ? "(" : strstr(en, "bmatrix") ? "[" : !strncmp(en, "cases", 5) ? "{ " : "") : (strstr(en, "pmatrix") ? ")" : strstr(en, "bmatrix") ? "]" : ""));
		free(e.s);
		return;
	}
	if (IS("phantom") || IS("hphantom") || IS("vphantom") || IS("label") || IS("tag")) { sb_t x = { 0 }; arg(p, &x); free(x.s); return; }
	if (IS("overset") || IS("underset") || IS("stackrel")) { sb_t x = { 0 }; arg(p, &x); free(x.s); arg(p, o); return; }
	if (IS("hspace") || IS("vspace") || IS("mkern") || IS("kern")) { sb_t x = { 0 }; arg(p, &x); free(x.s); sb_put(o, " "); return; }
	if (IS("pmod")) { sb_put(o, " (mod "); arg(p, o); sb_put(o, ")"); return; }
	if (IS("bmod") || IS("mod")) { sb_put(o, " mod"); return; }
#undef IS
	sb_add(o, s, l); /* \sin \log \max …, and anything unknown, read as their name */
}

static void seq(P *p, sb_t *o, char stop) {
	while (*p->p) {
		char c = *p->p;
		if (stop && c == stop) { p->p++; return; }
		if (c == '{') { p->p++; seq(p, o, '}'); continue; }
		if (c == '}') { p->p++; continue; }
		if (c == '^' || c == '_') {
			p->p++;
			sb_t t = { 0 };
			arg(p, &t);
			script(o, sb_take(&t), c == '^');
			free(t.s);
			continue;
		}
		if (c == '\\') { cmd(p, o); continue; }
		if (c == '&' || c == '~') { p->p++; sb_put(o, " "); continue; }
		if (c == '\'') { p->p++; sb_put(o, "′"); continue; }
		if (isspace((unsigned char)c)) {
			while (isspace((unsigned char)*p->p)) p->p++;
			if (o->n && o->s[o->n - 1] != ' ' && o->s[o->n - 1] != '\n') sb_put(o, " ");
			continue;
		}
		size_t l = u8len(p->p);
		sb_add(o, p->p, l);
		p->p += l;
	}
}

char *math_unicode(const char *tex) {
	P p = { tex ? tex : "" };
	sb_t o = { 0 };
	seq(&p, &o, 0);
	char *s = sb_take(&o);
	size_t a = 0, z = o.n; /* trim */
	while (a < z && (s[a] == ' ' || s[a] == '\n')) a++;
	while (z > a && (s[z - 1] == ' ' || s[z - 1] == '\n')) z--;
	memmove(s, s + a, z - a);
	s[z - a] = 0;
	return s;
}

/* ── stream filter ─────────────────────────────────────────────────────── */

enum { TEXT, INLINE, DISPLAY, CODE, FENCE };

static void emit(math_stream_t *m, const char *s, size_t n) {
	if (!n) return;
	if (m->eat_nl) { m->eat_nl = 0; if (*s == '\n') { s++; n--; if (!n) return; } }
	m->out(s, n);
	m->bol = s[n - 1] == '\n';
}

static void buf_add(math_stream_t *m, char c) {
	if (m->n + 2 > m->cap) { m->cap = m->cap ? m->cap * 2 : 128; m->buf = realloc(m->buf, m->cap); }
	m->buf[m->n++] = c;
	m->buf[m->n] = 0;
}

/* Give up on a math span: print it exactly as it came. */
static void abandon(math_stream_t *m) {
	emit(m, m->st == DISPLAY ? "$$" : "$", m->st == DISPLAY ? 2 : 1);
	emit(m, m->buf ? m->buf : "", m->n);
	if (m->close) emit(m, "$", 1);
	m->n = 0;
	m->close = 0;
	m->st = TEXT;
}

static void finish(math_stream_t *m) {
	buf_add(m, 0);
	m->n--;
	char *u = NULL;
	if (m->st == INLINE || m->oneline) {
		u = math_unicode(m->buf);
		if (m->oneline) for (char *c = u; *c; c++) if (*c == '\n') *c = ' ';
		emit(m, u, strlen(u));
	} else {
		if (!m->bol) emit(m, "\n", 1);
		if (math_display && !math_display(m->buf)) m->bol = 1;
		else {
			u = math_unicode(m->buf);
			sb_t ind = { 0 };
			sb_put(&ind, "    ");
			for (const char *c = u; *c; c++) { sb_add(&ind, c, 1); if (*c == '\n') sb_put(&ind, "    "); }
			sb_put(&ind, "\n");
			emit(m, ind.s, ind.n);
			free(ind.s);
		}
		m->eat_nl = 1;
	}
	free(u);
	m->n = 0;
	m->close = 0;
	m->st = TEXT;
}

/* A run of backticks ended: three or more at a line start open or close a fence. */
static void ticks_done(math_stream_t *m) {
	int t = m->ticks, at_bol = m->bol;
	m->ticks = 0;
	char run[64];
	int k = t < 63 ? t : 63;
	memset(run, '`', (size_t)k);
	emit(m, run, (size_t)k);
	if (t >= 3 && at_bol) m->st = m->st == FENCE ? TEXT : m->st == TEXT ? FENCE : m->st;
	else if (m->st != FENCE && (t & 1)) m->st = m->st == CODE ? TEXT : CODE;
}

static void step(math_stream_t *m, char c) {
	if (m->ticks && c != '`') ticks_done(m);
	if (c == '`' && (m->st == TEXT || m->st == CODE || m->st == FENCE)) {
		if (m->bs) { m->bs = 0; emit(m, "\\`", 2); return; }
		if (m->dollar) { m->dollar = 0; emit(m, "$", 1); }
		m->ticks++; /* printed when the run ends, so a fence is judged by where it began */
		return;
	}
	switch (m->st) {
	case CODE: case FENCE:
		emit(m, &c, 1);
		return;
	case TEXT:
		if (m->dollar == 1) {
			m->dollar = 0;
			if (c == '$') { m->st = DISPLAY; return; }
			if (c == ' ' || c == '\t' || c == '\n' || c == 0) { emit(m, "$", 1); break; }
			m->st = INLINE;
			step(m, c);
			return;
		}
		if (m->bs) {
			m->bs = 0;
			if (c == '$') { emit(m, "$", 1); return; }
			if (c == 'n') { emit(m, "\n", 1); return; }
			emit(m, "\\", 1);
		}
		if (c == '\\') { m->bs = 1; return; }
		if (c == '$') { m->dollar = 1; return; }
		break;
	case INLINE:
		if (m->close) { /* a closing $ is real unless a digit follows ($5 and $10) */
			if (isdigit((unsigned char)c)) { m->close = 0; buf_add(m, '$'); }
			else { finish(m); step(m, c); return; }
		}
		if (m->bs) { m->bs = 0; buf_add(m, c); return; }
		if (c == '\n' || m->n > 1024) { abandon(m); step(m, c); return; }
		if (c == '\\') { m->bs = 1; buf_add(m, c); return; }
		if (c == '$') {
			if (m->n && (m->buf[m->n - 1] == ' ' || m->buf[m->n - 1] == '\t')) { abandon(m); m->dollar = 1; return; }
			m->close = 1;
			return;
		}
		buf_add(m, c);
		return;
	case DISPLAY:
		if (m->close) {
			m->close = 0;
			if (c == '$') { finish(m); return; }
			buf_add(m, '$');
		}
		if (m->bs) { m->bs = 0; buf_add(m, c); return; }
		if (m->n > 16384) { abandon(m); step(m, c); return; }
		if (c == '\\') { m->bs = 1; buf_add(m, c); return; }
		if (c == '$') { m->close = 1; return; }
		buf_add(m, c);
		return;
	}
	emit(m, &c, 1);
}

void math_feed(math_stream_t *m, const char *s) {
	for (; s && *s; s++) step(m, *s);
}

void math_flush(math_stream_t *m) {
	if (m->ticks) ticks_done(m);
	if (m->bs) { m->bs = 0; if (m->st == TEXT) emit(m, "\\", 1); }
	if (m->dollar) { m->dollar = 0; emit(m, "$", 1); }
	if (m->st == INLINE && m->close) finish(m);
	else if (m->st == INLINE || m->st == DISPLAY) abandon(m);
	m->st = TEXT;
	m->eat_nl = 0;
	m->bol = 1;
	free(m->buf);
	m->buf = NULL;
	m->n = m->cap = 0;
}

static void to_stdout(const char *s, size_t n) { fwrite(s, 1, n, stdout); }

void math_puts(const char *s) {
	math_stream_t m = { .out = to_stdout, .bol = 1 };
	math_feed(&m, s);
	math_flush(&m);
}

static __thread sb_t *text_sink; /* per thread: plan views render on the MCP thread too */
static void to_sb(const char *s, size_t n) { sb_add(text_sink, s, n); }

char *math_text(const char *s) {
	sb_t o = { 0 };
	math_stream_t m = { .out = to_sb, .bol = 1, .oneline = 1 };
	sb_t *prev = text_sink;
	text_sink = &o;
	math_feed(&m, s);
	math_flush(&m);
	text_sink = prev;
	return sb_take(&o);
}
