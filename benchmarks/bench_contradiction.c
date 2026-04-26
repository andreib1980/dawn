/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * By contributing to this project, you agree to license your contributions
 * under the GPLv3 (or any later version) or any future licenses chosen by
 * the project author(s).
 *
 * Contradiction Detection Experiment
 *
 * Standalone benchmark that tests whether cosine similarity of sentence
 * embeddings, combined with Jaccard word-overlap, can distinguish
 * contradictory personal facts from paraphrases, related facts, and
 * unrelated facts.  Produces per-pair metrics, summary statistics with
 * 95% confidence intervals, threshold sweep with precision/recall/F1,
 * Cohen's d effect sizes, and per-category recall analysis.
 *
 * Usage:
 *   bench_contradiction [--provider onnx] [--csv results.csv] [--verbose]
 */

#include <ctype.h>
#include <getopt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/dawn_config.h"
#include "contradiction_pairs.h"
#include "core/embedding_engine.h"
#include "memory/memory_similarity.h"

/* =============================================================================
 * Extern Globals (defined in bench_contradiction_stub.c)
 * ============================================================================= */

extern dawn_config_t g_config;
extern secrets_config_t g_secrets;

/* =============================================================================
 * Constants
 * ============================================================================= */

#define MAX_UNIQUE_FACTS 512
#define MAX_DIMS 2048
#define MAX_CATEGORIES 32
#define LINE_WIDTH 100

/* Label codes for fast comparison */
#define LABEL_CONTRADICTION 0
#define LABEL_PARAPHRASE 1
#define LABEL_IDENTICAL 2
#define LABEL_RELATED 3
#define LABEL_UNRELATED 4
#define LABEL_COUNT 5

static const char *LABEL_NAMES[LABEL_COUNT] = { "contradiction", "paraphrase", "identical",
                                                "related", "unrelated" };

/* =============================================================================
 * Per-pair result
 * ============================================================================= */

typedef struct {
   int pair_idx;
   int label;
   const char *category;
   float cosine;
   float jaccard;
   float l2_dist;
   float unit_l2;
   float word_overlap;
} pair_result_t;

/* =============================================================================
 * Per-label accumulator
 * ============================================================================= */

typedef struct {
   int count;
   double cos_sum;
   double cos_sq_sum;
   double jac_sum;
   double jac_sq_sum;
   double l2_sum;
   double l2_sq_sum;
} label_stats_t;

/* =============================================================================
 * Per-category accumulator (contradictions only)
 * ============================================================================= */

typedef struct {
   char name[64];
   int count;
   double cos_sum;
   double cos_sq_sum;
   double jac_sum;
   double jac_sq_sum;
   int detected; /* count detected at best threshold */
} cat_stats_t;

/* =============================================================================
 * Unique fact cache (embed each text once)
 * ============================================================================= */

typedef struct {
   const char *text;
   float embedding[MAX_DIMS];
   float norm;
   int dims;
} fact_embedding_t;

static fact_embedding_t s_facts[MAX_UNIQUE_FACTS];
static int s_fact_count = 0;

static int label_code(const char *label) {
   for (int i = 0; i < LABEL_COUNT; i++) {
      if (strcmp(label, LABEL_NAMES[i]) == 0)
         return i;
   }
   return -1;
}

/* =============================================================================
 * Find or embed a unique fact
 * ============================================================================= */

static fact_embedding_t *get_fact_embedding(const char *text) {
   for (int i = 0; i < s_fact_count; i++) {
      if (strcmp(s_facts[i].text, text) == 0)
         return &s_facts[i];
   }

   if (s_fact_count >= MAX_UNIQUE_FACTS) {
      fprintf(stderr, "ERROR: exceeded MAX_UNIQUE_FACTS (%d)\n", MAX_UNIQUE_FACTS);
      return NULL;
   }

   fact_embedding_t *f = &s_facts[s_fact_count];
   f->text = text;
   f->dims = 0;

   if (embedding_engine_embed(text, f->embedding, MAX_DIMS, &f->dims) != 0) {
      fprintf(stderr, "ERROR: failed to embed: %s\n", text);
      return NULL;
   }

   f->norm = embedding_engine_l2_norm(f->embedding, f->dims);
   s_fact_count++;
   return f;
}

/* =============================================================================
 * Metric computation
 * ============================================================================= */

static float compute_l2(const float *a, const float *b, int dims) {
   float sum = 0.0f;
   for (int i = 0; i < dims; i++) {
      float d = a[i] - b[i];
      sum += d * d;
   }
   return sqrtf(sum);
}

static float compute_unit_l2(const float *a, const float *b, int dims, float norm_a, float norm_b) {
   if (norm_a < 1e-8f || norm_b < 1e-8f)
      return 0.0f;

   float sum = 0.0f;
   for (int i = 0; i < dims; i++) {
      float d = a[i] / norm_a - b[i] / norm_b;
      sum += d * d;
   }
   return sqrtf(sum);
}

static float compute_word_overlap(const char *a, const char *b) {
   char buf_a[512], buf_b[512];
   snprintf(buf_a, sizeof(buf_a), "%s", a);
   snprintf(buf_b, sizeof(buf_b), "%s", b);

   /* Lowercase both */
   for (int i = 0; buf_a[i]; i++)
      buf_a[i] = (char)tolower((unsigned char)buf_a[i]);
   for (int i = 0; buf_b[i]; i++)
      buf_b[i] = (char)tolower((unsigned char)buf_b[i]);

   /* Tokenize A */
   char *words_a[64];
   int count_a = 0;
   char *save_a = NULL;
   char *tok = strtok_r(buf_a, " \t\n\r.,;:!?\"'()-", &save_a);
   while (tok && count_a < 64) {
      if (strlen(tok) > 1)
         words_a[count_a++] = tok;
      tok = strtok_r(NULL, " \t\n\r.,;:!?\"'()-", &save_a);
   }

   /* Tokenize B */
   char *words_b[64];
   int count_b = 0;
   char *save_b = NULL;
   tok = strtok_r(buf_b, " \t\n\r.,;:!?\"'()-", &save_b);
   while (tok && count_b < 64) {
      if (strlen(tok) > 1)
         words_b[count_b++] = tok;
      tok = strtok_r(NULL, " \t\n\r.,;:!?\"'()-", &save_b);
   }

   if (count_a == 0 || count_b == 0)
      return 0.0f;

   int common = 0;
   for (int i = 0; i < count_a; i++) {
      for (int j = 0; j < count_b; j++) {
         if (strcmp(words_a[i], words_b[j]) == 0) {
            common++;
            break;
         }
      }
   }

   int shorter = count_a < count_b ? count_a : count_b;
   return (float)common / (float)shorter;
}

/* =============================================================================
 * Statistical helpers
 * ============================================================================= */

static void stats_mean_std(double sum, double sq_sum, int n, double *mean, double *std) {
   if (n == 0) {
      *mean = 0.0;
      *std = 0.0;
      return;
   }
   *mean = sum / n;
   double variance = sq_sum / n - (*mean) * (*mean);
   *std = variance > 0.0 ? sqrt(variance) : 0.0;
}

static double cohens_d(double mean_a, double std_a, int n_a, double mean_b, double std_b, int n_b) {
   double pooled_var = ((n_a - 1) * std_a * std_a + (n_b - 1) * std_b * std_b) / (n_a + n_b - 2);
   double pooled_std = sqrt(pooled_var);
   if (pooled_std < 1e-10)
      return 0.0;
   return fabs(mean_a - mean_b) / pooled_std;
}

static const char *effect_label(double d) {
   if (d >= 0.8)
      return "large";
   if (d >= 0.5)
      return "medium";
   return "small";
}

/* =============================================================================
 * Separator
 * ============================================================================= */

static void print_sep(void) {
   for (int i = 0; i < LINE_WIDTH; i++)
      putchar('=');
   putchar('\n');
}

/* =============================================================================
 * Main
 * ============================================================================= */

int main(int argc, char *argv[]) {
   const char *provider = "onnx";
   const char *model = "";
   const char *endpoint = "";
   const char *api_key = "";
   const char *csv_path = NULL;
   int verbose = 0;
   int sweep_only = 0;

   static struct option long_options[] = { { "provider", required_argument, NULL, 'p' },
                                           { "model", required_argument, NULL, 'm' },
                                           { "endpoint", required_argument, NULL, 'e' },
                                           { "api-key", required_argument, NULL, 'k' },
                                           { "csv", required_argument, NULL, 'c' },
                                           { "verbose", no_argument, NULL, 'v' },
                                           { "sweep-only", no_argument, NULL, 's' },
                                           { "help", no_argument, NULL, 'h' },
                                           { NULL, 0, NULL, 0 } };

   int opt;
   while ((opt = getopt_long(argc, argv, "p:m:e:k:c:vsh", long_options, NULL)) != -1) {
      switch (opt) {
         case 'p':
            provider = optarg;
            break;
         case 'm':
            model = optarg;
            break;
         case 'e':
            endpoint = optarg;
            break;
         case 'k':
            api_key = optarg;
            break;
         case 'c':
            csv_path = optarg;
            break;
         case 'v':
            verbose = 1;
            break;
         case 's':
            sweep_only = 1;
            break;
         case 'h':
            printf("Usage: %s [options]\n"
                   "  --provider <onnx|ollama|openai>  Embedding provider (default: onnx)\n"
                   "  --model <name>                   Model name for HTTP providers\n"
                   "  --endpoint <url>                 Endpoint URL\n"
                   "  --api-key <key>                  API key\n"
                   "  --csv <path>                     Write per-pair CSV\n"
                   "  --verbose                        Print every pair\n"
                   "  --sweep-only                     Skip per-pair, just analysis\n",
                   argv[0]);
            return 0;
         default:
            return 1;
      }
   }

   /* ── Initialize embedding engine ── */
   memset(&g_config, 0, sizeof(g_config));
   memset(&g_secrets, 0, sizeof(g_secrets));

   snprintf(g_config.memory.embedding_provider, sizeof(g_config.memory.embedding_provider), "%s",
            provider);
   snprintf(g_config.memory.embedding_model, sizeof(g_config.memory.embedding_model), "%s", model);
   snprintf(g_config.memory.embedding_endpoint, sizeof(g_config.memory.embedding_endpoint), "%s",
            endpoint);
   snprintf(g_secrets.embedding_api_key, sizeof(g_secrets.embedding_api_key), "%s", api_key);

   print_sep();
   printf("Contradiction Detection Experiment\n");
   printf("Provider: %s  Model: %s  Pairs: %d\n", provider, model[0] ? model : "(default)",
          (int)FACT_PAIR_COUNT);
   print_sep();

   if (embedding_engine_init() != 0) {
      fprintf(stderr, "FATAL: failed to initialize embedding engine (provider=%s)\n", provider);
      return 1;
   }

   int dims = embedding_engine_dims();
   printf("Embedding dimensions: %d\n\n", dims);

   /* ── Compute metrics for all pairs ── */
   pair_result_t results[FACT_PAIR_COUNT];
   label_stats_t lstats[LABEL_COUNT];
   memset(lstats, 0, sizeof(lstats));

   cat_stats_t cats[MAX_CATEGORIES];
   int cat_count = 0;

   int embed_failures = 0;

   for (int i = 0; i < (int)FACT_PAIR_COUNT; i++) {
      const fact_pair_t *p = &FACT_PAIRS[i];

      fact_embedding_t *ea = get_fact_embedding(p->fact_a);
      fact_embedding_t *eb = get_fact_embedding(p->fact_b);

      if (!ea || !eb) {
         embed_failures++;
         memset(&results[i], 0, sizeof(pair_result_t));
         results[i].pair_idx = i;
         results[i].label = -1;
         continue;
      }

      int lc = label_code(p->label);
      float cosine = embedding_engine_cosine_with_norms(ea->embedding, eb->embedding, dims,
                                                        ea->norm, eb->norm);
      float jaccard = memory_jaccard_similarity(p->fact_a, p->fact_b);
      float l2 = compute_l2(ea->embedding, eb->embedding, dims);
      float ul2 = compute_unit_l2(ea->embedding, eb->embedding, dims, ea->norm, eb->norm);
      float woverlap = compute_word_overlap(p->fact_a, p->fact_b);

      results[i].pair_idx = i;
      results[i].label = lc;
      results[i].category = p->category;
      results[i].cosine = cosine;
      results[i].jaccard = jaccard;
      results[i].l2_dist = l2;
      results[i].unit_l2 = ul2;
      results[i].word_overlap = woverlap;

      /* Accumulate label stats */
      if (lc >= 0 && lc < LABEL_COUNT) {
         label_stats_t *ls = &lstats[lc];
         ls->count++;
         ls->cos_sum += cosine;
         ls->cos_sq_sum += cosine * cosine;
         ls->jac_sum += jaccard;
         ls->jac_sq_sum += jaccard * jaccard;
         ls->l2_sum += l2;
         ls->l2_sq_sum += l2 * l2;
      }

      /* Accumulate per-category stats (contradictions only) */
      if (lc == LABEL_CONTRADICTION) {
         int ci = -1;
         for (int c = 0; c < cat_count; c++) {
            if (strcmp(cats[c].name, p->category) == 0) {
               ci = c;
               break;
            }
         }
         if (ci < 0 && cat_count < MAX_CATEGORIES) {
            ci = cat_count++;
            snprintf(cats[ci].name, sizeof(cats[ci].name), "%s", p->category);
            cats[ci].count = 0;
            cats[ci].cos_sum = 0.0;
            cats[ci].cos_sq_sum = 0.0;
            cats[ci].jac_sum = 0.0;
            cats[ci].jac_sq_sum = 0.0;
            cats[ci].detected = 0;
         }
         if (ci >= 0) {
            cats[ci].count++;
            cats[ci].cos_sum += cosine;
            cats[ci].cos_sq_sum += cosine * cosine;
            cats[ci].jac_sum += jaccard;
            cats[ci].jac_sq_sum += jaccard * jaccard;
         }
      }
   }

   printf("Unique facts embedded: %d  Failures: %d\n\n", s_fact_count, embed_failures);

   /* ── Per-pair output ── */
   if (verbose && !sweep_only) {
      printf("%-4s  %-14s %-12s %7s %7s %7s %7s %7s\n", "PAIR", "LABEL", "CATEGORY", "COSINE",
             "JACCARD", "L2", "UL2", "WOVLAP");
      for (int i = 0; i < LINE_WIDTH; i++)
         putchar('-');
      putchar('\n');

      for (int i = 0; i < (int)FACT_PAIR_COUNT; i++) {
         pair_result_t *r = &results[i];
         if (r->label < 0)
            continue;
         printf("%4d  %-14s %-12s %7.3f %7.3f %7.3f %7.3f %7.3f\n", i + 1, LABEL_NAMES[r->label],
                r->category, r->cosine, r->jaccard, r->l2_dist, r->unit_l2, r->word_overlap);
      }
      putchar('\n');
   }

   /* ── Per-label summary ── */
   print_sep();
   printf("PER-LABEL SUMMARY (95%% CI)\n");
   print_sep();
   printf("%-14s %4s  %9s %-16s  %9s %-16s  %7s\n", "LABEL", "N", "COS_MEAN", "COS_95CI",
          "JAC_MEAN", "JAC_95CI", "L2_MEAN");

   double cos_means[LABEL_COUNT], cos_stds[LABEL_COUNT];
   double jac_means[LABEL_COUNT], jac_stds[LABEL_COUNT];

   for (int i = 0; i < LABEL_COUNT; i++) {
      label_stats_t *ls = &lstats[i];
      double cm, cs, jm, js, lm, ls2;
      stats_mean_std(ls->cos_sum, ls->cos_sq_sum, ls->count, &cm, &cs);
      stats_mean_std(ls->jac_sum, ls->jac_sq_sum, ls->count, &jm, &js);
      stats_mean_std(ls->l2_sum, ls->l2_sq_sum, ls->count, &lm, &ls2);

      cos_means[i] = cm;
      cos_stds[i] = cs;
      jac_means[i] = jm;
      jac_stds[i] = js;

      double cos_ci = ls->count > 1 ? 1.96 * cs / sqrt(ls->count) : 0.0;
      double jac_ci = ls->count > 1 ? 1.96 * js / sqrt(ls->count) : 0.0;

      char cos_ci_str[32], jac_ci_str[32];
      snprintf(cos_ci_str, sizeof(cos_ci_str), "[%.3f, %.3f]", cm - cos_ci, cm + cos_ci);
      snprintf(jac_ci_str, sizeof(jac_ci_str), "[%.3f, %.3f]", jm - jac_ci, jm + jac_ci);

      printf("%-14s %4d  %9.4f %-16s  %9.4f %-16s  %7.4f\n", LABEL_NAMES[i], ls->count, cm,
             cos_ci_str, jm, jac_ci_str, lm);
   }
   putchar('\n');

   /* ── Per-category breakdown (contradictions) ── */
   print_sep();
   printf("PER-CATEGORY BREAKDOWN (contradictions only)\n");
   print_sep();
   printf("%-14s %3s  %8s %8s  %8s %8s\n", "CATEGORY", "N", "COS_MEAN", "COS_STD", "JAC_MEAN",
          "JAC_STD");

   for (int i = 0; i < cat_count; i++) {
      double cm, cs, jm, js;
      stats_mean_std(cats[i].cos_sum, cats[i].cos_sq_sum, cats[i].count, &cm, &cs);
      stats_mean_std(cats[i].jac_sum, cats[i].jac_sq_sum, cats[i].count, &jm, &js);
      printf("%-14s %3d  %8.4f %8.4f  %8.4f %8.4f\n", cats[i].name, cats[i].count, cm, cs, jm, js);
   }
   putchar('\n');

   /* ── Threshold sweep ── */
   print_sep();
   printf("THRESHOLD SWEEP (contradiction = positive)\n");
   print_sep();
   printf("%-10s %-10s %8s %8s %8s %8s  %4s %4s %4s %4s\n", "COS_THRESH", "JAC_CEIL", "PREC",
          "RECALL", "F1", "ACC", "TP", "FP", "TN", "FN");

   float best_f1 = 0.0f;
   float best_cos_thresh = 0.0f;
   float best_jac_ceil = 0.0f;
   float best_cos_only_f1 = 0.0f;
   float best_cos_only_thresh = 0.0f;

   /* Sweep cosine thresholds */
   for (int ci = 0; ci <= 18; ci++) {
      float cos_thresh = 0.50f + ci * 0.025f;

      /* Sweep Jaccard ceilings (1.01 = no Jaccard filter) */
      float jac_ceilings[] = { 1.01f, 0.40f, 0.45f, 0.50f, 0.55f, 0.60f, 0.65f,
                               0.70f, 0.75f, 0.80f, 0.85f, 0.90f, 0.95f };
      int jac_count = sizeof(jac_ceilings) / sizeof(jac_ceilings[0]);

      for (int ji = 0; ji < jac_count; ji++) {
         float jac_ceil = jac_ceilings[ji];
         int tp = 0, fp = 0, tn = 0, fn = 0;

         for (int i = 0; i < (int)FACT_PAIR_COUNT; i++) {
            if (results[i].label < 0)
               continue;

            int is_contradiction = (results[i].label == LABEL_CONTRADICTION);
            int predicted = (results[i].cosine >= cos_thresh && results[i].jaccard < jac_ceil);

            if (predicted && is_contradiction)
               tp++;
            else if (predicted && !is_contradiction)
               fp++;
            else if (!predicted && is_contradiction)
               fn++;
            else
               tn++;
         }

         float prec = tp + fp > 0 ? (float)tp / (tp + fp) : 0.0f;
         float recall = tp + fn > 0 ? (float)tp / (tp + fn) : 0.0f;
         float f1 = prec + recall > 0 ? 2 * prec * recall / (prec + recall) : 0.0f;
         float acc = (float)(tp + tn) / (tp + fp + tn + fn);

         /* Track best overall */
         if (f1 > best_f1) {
            best_f1 = f1;
            best_cos_thresh = cos_thresh;
            best_jac_ceil = jac_ceil;
         }

         /* Track best cosine-only (no Jaccard filter) */
         if (jac_ceil > 1.0f && f1 > best_cos_only_f1) {
            best_cos_only_f1 = f1;
            best_cos_only_thresh = cos_thresh;
         }

         /* Only print rows with non-trivial F1 */
         if (f1 >= 0.20f) {
            const char *jac_label = jac_ceil > 1.0f ? "none" : "";
            if (jac_ceil > 1.0f) {
               printf("%-10.3f %-10s %8.3f %8.3f %8.3f %8.3f  %4d %4d %4d %4d\n", cos_thresh,
                      "none", prec, recall, f1, acc, tp, fp, tn, fn);
            } else {
               printf("%-10.3f %-10.3f %8.3f %8.3f %8.3f %8.3f  %4d %4d %4d %4d\n", cos_thresh,
                      jac_ceil, prec, recall, f1, acc, tp, fp, tn, fn);
            }
            (void)jac_label;
         }
      }
   }

   putchar('\n');
   if (best_jac_ceil > 1.0f) {
      printf("BEST F1:          cosine >= %.3f, no Jaccard filter -> F1 = %.3f\n", best_cos_thresh,
             best_f1);
   } else {
      printf("BEST F1:          cosine >= %.3f, jaccard < %.3f -> F1 = %.3f\n", best_cos_thresh,
             best_jac_ceil, best_f1);
   }
   printf("BEST COSINE-ONLY: cosine >= %.3f -> F1 = %.3f\n", best_cos_only_thresh,
          best_cos_only_f1);
   printf("JACCARD DELTA:    %.3f\n", best_f1 - best_cos_only_f1);
   putchar('\n');

   /* ── Cohen's d effect sizes ── */
   print_sep();
   printf("EFFECT SIZES (Cohen's d: contradiction vs. each label)\n");
   print_sep();
   printf("%-14s %8s %-8s  %8s %-8s\n", "VS_LABEL", "COS_D", "COS_EFF", "JAC_D", "JAC_EFF");

   for (int i = 1; i < LABEL_COUNT; i++) {
      double cd = cohens_d(cos_means[LABEL_CONTRADICTION], cos_stds[LABEL_CONTRADICTION],
                           lstats[LABEL_CONTRADICTION].count, cos_means[i], cos_stds[i],
                           lstats[i].count);
      double jd = cohens_d(jac_means[LABEL_CONTRADICTION], jac_stds[LABEL_CONTRADICTION],
                           lstats[LABEL_CONTRADICTION].count, jac_means[i], jac_stds[i],
                           lstats[i].count);
      printf("%-14s %8.3f %-8s  %8.3f %-8s\n", LABEL_NAMES[i], cd, effect_label(cd), jd,
             effect_label(jd));
   }
   putchar('\n');

   /* ── Per-category recall at best threshold ── */
   print_sep();
   printf("PER-CATEGORY RECALL at best threshold (cosine >= %.3f", best_cos_thresh);
   if (best_jac_ceil <= 1.0f)
      printf(", jaccard < %.3f", best_jac_ceil);
   printf(")\n");
   print_sep();
   printf("%-14s %3s  %4s  %6s\n", "CATEGORY", "N", "TP", "RECALL");

   for (int ci = 0; ci < cat_count; ci++) {
      int cat_tp = 0;
      for (int i = 0; i < (int)FACT_PAIR_COUNT; i++) {
         if (results[i].label != LABEL_CONTRADICTION)
            continue;
         if (strcmp(results[i].category, cats[ci].name) != 0)
            continue;
         if (results[i].cosine >= best_cos_thresh && results[i].jaccard < best_jac_ceil) {
            cat_tp++;
         }
      }
      float recall = cats[ci].count > 0 ? (float)cat_tp / cats[ci].count : 0.0f;
      printf("%-14s %3d  %4d  %6.3f%s\n", cats[ci].name, cats[ci].count, cat_tp, recall,
             recall < 0.60f ? "  *** LOW ***" : "");
   }
   putchar('\n');

   /* ── Write CSV ── */
   if (csv_path) {
      FILE *fp = fopen(csv_path, "w");
      if (!fp) {
         fprintf(stderr, "ERROR: cannot open %s for writing\n", csv_path);
      } else {
         fprintf(fp,
                 "id,fact_a,fact_b,label,category,cosine,jaccard,l2_dist,unit_l2,word_overlap\n");
         for (int i = 0; i < (int)FACT_PAIR_COUNT; i++) {
            pair_result_t *r = &results[i];
            if (r->label < 0)
               continue;

            const char *fa = FACT_PAIRS[i].fact_a;
            const char *fb = FACT_PAIRS[i].fact_b;
            fprintf(fp, "%d,\"%s\",\"%s\",%s,%s,%.6f,%.6f,%.6f,%.6f,%.6f\n", i + 1, fa, fb,
                    LABEL_NAMES[r->label], r->category, r->cosine, r->jaccard, r->l2_dist,
                    r->unit_l2, r->word_overlap);
         }
         fclose(fp);
         printf("CSV written to: %s\n", csv_path);
      }
   }

   /* ── Summary ── */
   print_sep();
   printf("EXPERIMENT COMPLETE\n");
   printf("Pairs: %d  Labels: %d  Categories: %d  Dimensions: %d\n", (int)FACT_PAIR_COUNT,
          LABEL_COUNT, cat_count, dims);
   printf("Best F1: %.3f (cosine >= %.3f", best_f1, best_cos_thresh);
   if (best_jac_ceil <= 1.0f)
      printf(", jaccard < %.3f", best_jac_ceil);
   printf(")\n");
   printf("Jaccard improvement over cosine-only: %+.3f F1\n", best_f1 - best_cos_only_f1);
   print_sep();

   embedding_engine_cleanup();
   return 0;
}
