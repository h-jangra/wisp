#define _GNU_SOURCE
#include "ipc/frame.h"
#include <string.h>
#include <stdio.h>

/* Minimal strict JSON for the closed frame shape. Unknown keys are rejected:
 * the field set is fixed, so there is nothing to skip. */

struct jp {
	const char *p;
	const char *end;
};

static void jp_ws(struct jp *j)
{
	while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r'))
		j->p++;
}

static bool jp_char(struct jp *j, char c)
{
	if (j->p < j->end && *j->p == c) {
		j->p++;
		return true;
	}
	return false;
}

/* Decodes a JSON string into out (in place when out == j->p; escapes shrink). */
static bool jp_string(struct jp *j, char *out, size_t cap)
{
	if (!jp_char(j, '"'))
		return false;
	size_t o = 0;
	while (j->p < j->end && *j->p != '"') {
		unsigned char c = (unsigned char)*j->p;
		if (c < 0x20)
			return false;
		j->p++;
		if (c == '\\') {
			if (j->p >= j->end)
				return false;
			char e = *j->p++;
			switch (e) {
			case '"': case '\\': case '/': c = (unsigned char)e; break;
			case 'b': c = '\b'; break;
			case 'f': c = '\f'; break;
			case 'n': c = '\n'; break;
			case 'r': c = '\r'; break;
			case 't': c = '\t'; break;
			case 'u': {
				if (j->end - j->p < 4)
					return false;
				unsigned u = 0;
				for (int k = 0; k < 4; k++) {
					char h = *j->p++;
					u <<= 4;
					if (h >= '0' && h <= '9') u |= (unsigned)(h - '0');
					else if (h >= 'a' && h <= 'f') u |= (unsigned)(h - 'a' + 10);
					else if (h >= 'A' && h <= 'F') u |= (unsigned)(h - 'A' + 10);
					else return false;
				}
				if (u == 0)
					return false;
				if (u < 0x80) {
					if (o >= cap) return false;
					out[o++] = (char)u;
				} else if (u < 0x800) {
					if (o + 2 > cap) return false;
					out[o++] = (char)(0xC0 | (u >> 6));
					out[o++] = (char)(0x80 | (u & 0x3F));
				} else {
					if (o + 3 > cap) return false;
					out[o++] = (char)(0xE0 | (u >> 12));
					out[o++] = (char)(0x80 | ((u >> 6) & 0x3F));
					out[o++] = (char)(0x80 | (u & 0x3F));
				}
				continue;
			}
			default:
				return false;
			}
		}
		if (o >= cap)
			return false;
		out[o++] = (char)c;
	}
	if (j->p >= j->end)
		return false;
	j->p++; /* closing quote */
	if (o >= cap)
		return false;
	out[o] = '\0';
	return true;
}

static bool jp_u32(struct jp *j, uint32_t *out)
{
	unsigned long long v = 0;
	int digits = 0;
	while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
		v = v * 10 + (unsigned long long)(*j->p - '0');
		if (v > 0xFFFFFFFFULL)
			return false;
		digits++;
		j->p++;
	}
	if (!digits)
		return false;
	*out = (uint32_t)v;
	return true;
}

void ipc_frame_prefix(unsigned char out[4], size_t len)
{
	out[0] = (unsigned char)((len >> 24) & 0xFF);
	out[1] = (unsigned char)((len >> 16) & 0xFF);
	out[2] = (unsigned char)((len >> 8) & 0xFF);
	out[3] = (unsigned char)(len & 0xFF);
}

size_t ipc_frame_len(const unsigned char hdr[4])
{
	return ((size_t)hdr[0] << 24) | ((size_t)hdr[1] << 16) | ((size_t)hdr[2] << 8) | (size_t)hdr[3];
}

int ipc_json_encode_request(char *out, size_t cap, uint32_t gen, const char *verb, int argc, char *const *argv)
{
	if (!out || cap < 64 || !verb || argc < 0 || (argc > 0 && !argv))
		return -1;
	int n = snprintf(out, cap, "{\"gen\":%u,\"verb\":\"%s\",\"args\":[", gen, verb);
	if (n < 0 || (size_t)n >= cap)
		return -1;
	size_t o = (size_t)n;
	for (int i = 0; i < argc; i++) {
		const char *s = argv[i] ? argv[i] : "";
		if (i) {
			if (o + 1 >= cap) return -1;
			out[o++] = ',';
		}
		if (o + 1 >= cap) return -1;
		out[o++] = '"';
		for (const char *q = s; *q; q++) {
			unsigned char c = (unsigned char)*q;
			const char *rep = NULL;
			char u[8];
			switch (c) {
			case '"': rep = "\\\""; break;
			case '\\': rep = "\\\\"; break;
			case '\b': rep = "\\b"; break;
			case '\f': rep = "\\f"; break;
			case '\n': rep = "\\n"; break;
			case '\r': rep = "\\r"; break;
			case '\t': rep = "\\t"; break;
			default:
				if (c < 0x20) {
					snprintf(u, sizeof(u), "\\u%04x", c);
					rep = u;
				}
			}
			if (rep) {
				size_t l = strlen(rep);
				if (o + l + 2 >= cap) return -1;
				memcpy(out + o, rep, l);
				o += l;
			} else {
				if (o + 2 >= cap) return -1;
				out[o++] = (char)c;
			}
		}
		if (o + 1 >= cap) return -1;
		out[o++] = '"';
	}
	if (o + 3 >= cap) return -1;
	memcpy(out + o, "]}", 3);
	o += 2;
	out[o] = '\0';
	return (int)o;
}

int ipc_json_encode_response(char *out, size_t cap, uint32_t gen, bool ok, const char *text)
{
	if (!out || cap < 32 || !text)
		return -1;
	int n = snprintf(out, cap, "{\"gen\":%u,\"ok\":%s,\"text\":\"", gen, ok ? "true" : "false");
	if (n < 0 || (size_t)n >= cap)
		return -1;
	size_t o = (size_t)n;
	for (const char *s = text; *s; s++) {
		unsigned char c = (unsigned char)*s;
		const char *rep = NULL;
		char u[8];
		switch (c) {
		case '"': rep = "\\\""; break;
		case '\\': rep = "\\\\"; break;
		case '\b': rep = "\\b"; break;
		case '\f': rep = "\\f"; break;
		case '\n': rep = "\\n"; break;
		case '\r': rep = "\\r"; break;
		case '\t': rep = "\\t"; break;
		default:
			if (c < 0x20) {
				snprintf(u, sizeof(u), "\\u%04x", c);
				rep = u;
			}
		}
		if (rep) {
			size_t l = strlen(rep);
			if (o + l + 3 >= cap) return -1;
			memcpy(out + o, rep, l);
			o += l;
		} else {
			if (o + 4 >= cap) return -1;
			out[o++] = (char)c;
		}
	}
	if (o + 3 >= cap) return -1;
	memcpy(out + o, "\"}", 3);
	o += 2;
	out[o] = '\0';
	return (int)o;
}

static const char *const ipc_verbs[] = {
	"open", "close", "toggle", "reload", "list", "apps", "status", "load", "unload", "stop", "kill",
};

bool ipc_verb_valid(const char *verb)
{
	if (!verb)
		return false;
	for (size_t i = 0; i < sizeof(ipc_verbs) / sizeof(ipc_verbs[0]); i++)
		if (strcmp(verb, ipc_verbs[i]) == 0)
			return true;
	return false;
}

int ipc_json_decode_response(const char *payload, size_t len, uint32_t *gen, bool *ok,
			     char *text, size_t text_cap)
{
	if (!payload || len > IPC_MAX_FRAME || !gen || !ok || !text || !text_cap)
		return -1;
	*text = '\0';
	struct jp j = { payload, payload + len };
	jp_ws(&j);
	if (!jp_char(&j, '{'))
		return -1;
	bool have_gen = false, have_ok = false, have_text = false;
	jp_ws(&j);
	if (!jp_char(&j, '}')) {
		for (;;) {
			jp_ws(&j);
			char key[16];
			if (!jp_string(&j, key, sizeof(key)))
				return -1;
			jp_ws(&j);
			if (!jp_char(&j, ':'))
				return -1;
			jp_ws(&j);
			if (strcmp(key, "gen") == 0) {
				if (have_gen)
					return -1;
				if (!jp_u32(&j, gen))
					return -1;
				have_gen = true;
			} else if (strcmp(key, "ok") == 0) {
				if (have_ok)
					return -1;
				if (j.end - j.p >= 4 && memcmp(j.p, "true", 4) == 0) {
					*ok = true;
					j.p += 4;
				} else if (j.end - j.p >= 5 && memcmp(j.p, "false", 5) == 0) {
					*ok = false;
					j.p += 5;
				} else {
					return -1;
				}
				have_ok = true;
			} else if (strcmp(key, "text") == 0) {
				if (have_text)
					return -1;
				if (!jp_string(&j, text, text_cap))
					return -1;
				have_text = true;
			} else {
				return -1;
			}
			jp_ws(&j);
			if (jp_char(&j, ','))
				continue;
			if (jp_char(&j, '}'))
				break;
			return -1;
		}
	}
	jp_ws(&j);
	if (j.p != j.end)
		return -1;
	if (!have_gen || !have_ok || !have_text)
		return -1;
	return 0;
}

int ipc_frame_decode(char *payload, size_t len, uint32_t *gen, char *verb, size_t verb_cap,
		     char **args, int max_args, int *argc)
{
	if (!payload || len > IPC_MAX_FRAME || !gen || !verb || !argc)
		return -1;
	*argc = 0;
	struct jp j = { payload, payload + len };
	jp_ws(&j);
	if (!jp_char(&j, '{'))
		return -1;
	bool have_gen = false, have_verb = false;
	int args_n = 0;
	jp_ws(&j);
	if (!jp_char(&j, '}')) {
		for (;;) {
			jp_ws(&j);
			char key[16];
			if (!jp_string(&j, key, sizeof(key)))
				return -1;
			jp_ws(&j);
			if (!jp_char(&j, ':'))
				return -1;
			jp_ws(&j);
			if (strcmp(key, "gen") == 0) {
				if (have_gen)
					return -1;
				if (!jp_u32(&j, gen))
					return -1;
				have_gen = true;
			} else if (strcmp(key, "verb") == 0) {
				if (have_verb)
					return -1;
				if (!jp_string(&j, verb, verb_cap))
					return -1;
				have_verb = true;
			} else if (strcmp(key, "args") == 0) {
				if (!jp_char(&j, '['))
					return -1;
				jp_ws(&j);
				if (!jp_char(&j, ']')) {
					for (;;) {
						jp_ws(&j);
						if (args_n >= max_args)
							return -1;
						if (j.p + 1 >= j.end || *j.p != '"')
							return -1;
						args[args_n++] = (char *)(j.p + 1); /* unescape in place */
						if (!jp_string(&j, (char *)(j.p + 1), (size_t)(j.end - j.p)))
							return -1;
						jp_ws(&j);
						if (!jp_char(&j, ',')) {
							if (!jp_char(&j, ']'))
								return -1;
							break;
						}
					}
				}
			} else {
				return -1; /* closed shape */
			}
			jp_ws(&j);
			if (jp_char(&j, ','))
				continue;
			if (jp_char(&j, '}'))
				break;
			return -1;
		}
	}
	jp_ws(&j);
	if (j.p != j.end)
		return -1;
	if (!have_gen || !have_verb)
		return -1;
	*argc = args_n;
	return 0;
}
