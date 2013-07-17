#include <ctype.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <zlib.h>
#include "psmc.h"
#include "kseq.h"
KSEQ_INIT(gzFile, gzread)

#define DEFAULT_PATTERN "4+5*3+4"
#define PSMC_VERSION "0.6.4-r49"

static char conv_table[256] = {
	 2, 2, 2, 2,  2, 2, 2, 2,  2, 2, 2, 2,  2, 2, 2, 2,
	 2, 2, 2, 2,  2, 2, 2, 2,  2, 2, 2, 2,  2, 2, 2, 2,
	 2, 2, 2, 2,  2, 2, 2, 2,  2, 2, 2, 2,  2, 2, 2, 2,
	 0, 1, 2, 2,  2, 2, 2, 2,  2, 2, 2, 2,  2, 2, 2, 2,
	 2, 0, 2, 0,  2, 2, 2, 0,  2, 2, 2, 1,  2, 1, 2, 2,
	 2, 2, 1, 1,  0, 2, 2, 1,  2, 1, 2, 2,  2, 2, 2, 2,
	 2, 0, 2, 0,  2, 2, 2, 0,  2, 2, 2, 1,  2, 1, 2, 2,
	 2, 2, 1, 1,  0, 2, 2, 1,  2, 1, 2, 2,  2, 2, 2, 2,
	 2, 2, 2, 2,  2, 2, 2, 2,  2, 2, 2, 2,  2, 2, 2, 2,
	 2, 2, 2, 2,  2, 2, 2, 2,  2, 2, 2, 2,  2, 2, 2, 2,
	 2, 2, 2, 2,  2, 2, 2, 2,  2, 2, 2, 2,  2, 2, 2, 2,
	 2, 2, 2, 2,  2, 2, 2, 2,  2, 2, 2, 2,  2, 2, 2, 2,
	 2, 2, 2, 2,  2, 2, 2, 2,  2, 2, 2, 2,  2, 2, 2, 2,
	 2, 2, 2, 2,  2, 2, 2, 2,  2, 2, 2, 2,  2, 2, 2, 2,
	 2, 2, 2, 2,  2, 2, 2, 2,  2, 2, 2, 2,  2, 2, 2, 2,
	 2, 2, 2, 2,  2, 2, 2, 2,  2, 2, 2, 2,  2, 2, 2, 2
};

static void usage(psmc_par_t *p)
{
	fprintf(stderr, "\n");
	fprintf(stderr, "Program: psmc (Pairwise SMC Model)\n");
	fprintf(stderr, "Version: %s\n", PSMC_VERSION);
	fprintf(stderr, "Contact: <http://hengli.uservoice.com/>\n\n");
	fprintf(stderr, "Usage:   psmc [options] input.txt\n\n");
	fprintf(stderr, "Options: -p STR      pattern of parameters [%s]\n", DEFAULT_PATTERN);
	fprintf(stderr, "         -t FLOAT    maximum 2N0 coalescent time [%lg]\n", p->max_t);
	fprintf(stderr, "         -N INT      maximum number of iterations [%d]\n", p->n_iters);
	fprintf(stderr, "         -r FLOAT    initial theta/rho ratio [%lg]\n", p->tr_ratio);
	fprintf(stderr, "         -c FILE     CpG counts generated by cntcpg [null]\n");
//	fprintf(stderr, "         -l FLOAT    time skipping factor [0.1]\n"); // maybe not to change this
	fprintf(stderr, "         -o FILE     output file [stdout]\n");
	fprintf(stderr, "         -i FILE     input parameter file [null]\n");
//	fprintf(stderr, "         -I FLOAT    amplitude of random initialization for EM iteration [%lg]\n", p->ran_init);
	fprintf(stderr, "         -T FLOAT    initial divergence time; -1 to disable [-1]\n");
	fprintf(stderr, "         -b          bootstrap (input be preprocessed with split_psmcfa)\n");
	fprintf(stderr, "         -S          simulate sequence\n");
	fprintf(stderr, "         -d          perform decoding\n");
	fprintf(stderr, "         -D          print full posterior probabilities\n");
	fprintf(stderr, "\n");
	psmc_delete_par(p);
	exit(1);
}

// parse a pattern like "4+5*3+4"
// the returned array holds which parameters are linked together
// number of parameters and number of free parameters will be also returned

int *psmc_parse_pattern(const char *pattern, int *n_free, int *n_pars)
{
	char *q, *p, *tmp;
	int top = 0, *stack = (int*)malloc(sizeof(int) * 0x100);
	int *pars_map, k, l, i;
	p = q = tmp = strdup(pattern);
	k = 1;
	while (1) {
		assert(isdigit(*p) || *p == '*' || *p == '+' || *p == '\0'); // allowed characters
		if (*p == '+' || *p == '\0') {
			int is_end = (*p == 0)? 1 : 0;
			*p++ = '\0';
			l = atoi(q); q = p;
			for (i = 0; i < k; ++i) {
				stack[top++] = l;
				assert(top <= 0xff);
			}
			k = 1;
			if (is_end) break;
		} else if (*p == '*') {
			*p = '\0';
			k = atoi(q); // number of repeats
			*p++ = '*'; q = p;
		} else ++p;
	}
	for (k = l = 0; k != top; ++k) l += stack[k];
	*n_pars = l - 1; *n_free = top;
	pars_map = (int*)malloc(sizeof(int) * (*n_pars + 1));
	for (k = i = 0; k != top; ++k)
		for (l = 0; l < stack[k]; ++l)
			pars_map[i++] = k;
	free(tmp); free(stack);
	return pars_map;
}

// read the input

void psmc_read_seq(const char *fn, psmc_par_t *pp)
{
	kseq_t *ks;
	int i, j;
	gzFile fp;
	fp = strcmp(fn, "-")? gzopen(fn, "r") : gzdopen(fileno(stdin), "r");
	ks = kseq_init(fp);
	while (kseq_read(ks) >= 0) {
		char *s;
		int L_e = 0, n_e = 0, L = 0;
		if ((pp->n_seqs&0xff) == 0)
			pp->seqs = (psmc_seq_t*)realloc(pp->seqs, sizeof(psmc_seq_t) * (pp->n_seqs + 0x100));
		pp->seqs[pp->n_seqs].name = strdup(ks->name.s);
		s = pp->seqs[pp->n_seqs].seq = (char*)calloc(ks->seq.l + 1, 1);
		L = 0;
		for (j = 0; j != ks->seq.l; ++j) {
			char c = conv_table[(int)ks->seq.s[j]];
			s[L++] = c;
			if (c < 2) {
				++L_e;
				if (c == 1) ++n_e;
			}
		}
		pp->seqs[pp->n_seqs].L_e = L_e;
		pp->seqs[pp->n_seqs].n_e = n_e;
		pp->seqs[pp->n_seqs++].L = L;
	}
	kseq_destroy(ks);
	gzclose(fp);
	// count the sum of lengths and number of segsites
	pp->sum_n = pp->sum_L = 0;
	for (i = 0; i != pp->n_seqs; ++i) {
		pp->sum_n += pp->seqs[i].n_e;
		pp->sum_L += pp->seqs[i].L_e;
	}
}

// parse the command-line options

psmc_par_t *psmc_new_par()
{
	psmc_par_t *par;
	par = (psmc_par_t*)calloc(1, sizeof(psmc_par_t));
	par->n_iters = 30;
	par->fpout = stdout;
	par->max_t = 15.0;
	par->tr_ratio = 4.0;
	par->alpha = 0.1;
	par->dt0 = -1.0;
	return par;
}

void psmc_delete_par(psmc_par_t *par)
{
	int i;
	if (par == 0) return;
	free(par->par_map); free(par->inp_pa);
	free(par->pre_fn); free(par->pattern);
	fclose(par->fpout);
	for (i = 0; i != par->n_seqs; ++i) {
		free(par->seqs[i].name);
		free(par->seqs[i].seq);
	}
	if (par->fpcnt) fclose(par->fpcnt);
	free(par->seqs);
	free(par);
}

psmc_par_t *psmc_parse_cli(int argc, char *argv[])
{
	int c, is_bootstrap = 0;
	psmc_par_t *par;
	par = psmc_new_par();
	while ((c = getopt(argc, argv, "i:t:l:r:N:p:o:dI:c:bSDT:")) >= 0) {
		switch (c) {
		case 'S': par->flag |= PSMC_F_SIMU; break;
		case 'd': par->flag |= PSMC_F_DECODE; break;
		case 'D': par->flag |= PSMC_F_FULLDEC; break;
		case 'T': par->dt0 = atof(optarg); if (par->dt0 >= 0) par->flag |= PSMC_F_DIVERG; break;
		case 't': par->max_t = atof(optarg); break;
		case 'l': par->alpha = atof(optarg); break;
		case 'r': par->tr_ratio = atof(optarg); break;
		case 'N': par->n_iters = atoi(optarg); break;
		case 'i': par->pre_fn = strdup(optarg); break;
		case 'o': par->fpout = fopen(optarg, "w"); assert(par->fpout); break;
		case 'c': par->fpcnt = fopen(optarg, "rb"); assert(par->fpcnt); break;
		case 'p': par->pattern = strdup(optarg); break;
		case 'I': par->ran_init = atof(optarg); break;
		case 'b': is_bootstrap = 1; break;
		default:  usage(par);
		}
	}
	if (par->flag & PSMC_F_FULLDEC) par->flag |= PSMC_F_DECODE;
	if (optind == argc) usage(par);
	fprintf(par->fpout, "CC\n");
	fprintf(par->fpout, "CC\tBrief Description of the file format:\n");
	fprintf(par->fpout, "CC\t  CC  comments\n");
	fprintf(par->fpout, "CC\t  MM  useful-messages\n");
	fprintf(par->fpout, "CC\t  RD  round-of-iterations\n");
	fprintf(par->fpout, "CC\t  LL  \\log[P(sequence)]\n");
	fprintf(par->fpout, "CC\t  QD  Q-before-opt Q-after-opt\n");
	fprintf(par->fpout, "CC\t  TR  \\theta_0 \\rho_0\n");
	fprintf(par->fpout, "CC\t  RS  k t_k \\lambda_k \\pi_k \\sum_{l\\not=k}A_{kl} A_{kk}\n");
	fprintf(par->fpout, "CC\t  DC  begin end best-k t_k+\\Delta_k max-prob\n");
	fprintf(par->fpout, "CC\n");
	if (par->pre_fn) { // load parameters from input
		psmc_read_param(par);
	} else {
		if (par->pattern == 0) par->pattern = strdup(DEFAULT_PATTERN);
		par->par_map = psmc_parse_pattern(par->pattern, &par->n_free, &par->n);
	}
	fprintf(par->fpout, "MM\tVersion: %s\n", PSMC_VERSION);
	fprintf(par->fpout, "MM\tpattern:%s, n:%d, n_free_lambdas:%d\n", par->pattern, par->n, par->n_free);
	fprintf(par->fpout, "MM\tn_iterations:%d, skip:1, max_t:%g, theta/rho:%g\n",
			par->n_iters, par->max_t, par->tr_ratio);
	fprintf(par->fpout, "MM\tis_decoding:%d\n", (par->flag&PSMC_F_DECODE)? 1 : 0);
	psmc_read_seq(argv[optind], par);
	if (is_bootstrap) psmc_resamp(par); // resampling if required
	fprintf(par->fpout, "MM\tn_seqs:%d, sum_L:%lld, sum_n:%d\n", par->n_seqs, (long long)par->sum_L, par->sum_n);
	fflush(par->fpout);
	return par;
}
