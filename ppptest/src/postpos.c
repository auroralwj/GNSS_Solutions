/*------------------------------------------------------------------------------
* postpos.c : post-processing positioning
*
*          Copyright (C) 2007-2014 by T.TAKASU, All rights reserved.
*
* version : $Revision: 1.1 $ $Date: 2008/07/17 21:48:06 $
* history : 2007/05/08  1.0  new
*           2008/06/16  1.1  support binary inputs
*           2009/01/02  1.2  support new rtk positioing api
*           2009/09/03  1.3  fix bug on combined mode of moving-baseline
*           2009/12/04  1.4  fix bug on obs data buffer overflow
*           2010/07/26  1.5  support ppp-kinematic and ppp-static
*                            support multiple sessions
*                            support sbas positioning
*                            changed api:
*                                postpos()
*                            deleted api:
*                                postposopt()
*           2010/08/16  1.6  fix bug sbas message synchronization (2.4.0_p4)
*           2010/12/09  1.7  support qzss lex and ssr corrections
*           2011/02/07  1.8  fix bug on sbas navigation data conflict
*           2011/03/22  1.9  add function reading g_tec file
*           2011/08/20  1.10 fix bug on freez if solstatic=single and combined
*           2011/09/15  1.11 add function reading stec file
*           2012/02/01  1.12 support keyword expansion of rtcm ssr corrections
*           2013/03/11  1.13 add function reading otl and erp data
*           2014/06/29  1.14 fix problem on overflow of # of satellites
*           2015/03/23  1.15 fix bug on ant type replacement by rinex header
*                            fix bug on combined filter for moving-base mode
*-----------------------------------------------------------------------------*/
#include "rtklib.h"

static const char rcsid[] = "$Id: postpos.c,v 1.1 2008/07/17 21:48:06 ttaka Exp $";

#define MIN(x,y)    ((x)<(y)?(x):(y))
#define SQRT(x)     ((x)<=0.0?0.0:sqrt(x))

#define MAXPRCDAYS  100          /* max days of continuous processing */
#define MAXINFILE   1000         /* max number of input files */

/* constants/global variables ------------------------------------------------*/

static pcvs_t pcvss = { 0 };        /* receiver antenna parameters */
static pcvs_t pcvsr = { 0 };        /* satellite antenna parameters */
static obs_t obss = { 0 };          /* observation data */
static nav_t navs = { 0 };          /* navigation data */
static sbs_t sbss = { 0 };          /* sbas messages */
static lex_t lexs = { 0 };          /* lex messages */
static sta_t stas[MAXRCV];      /* station infomation */
static int nepoch = 0;            /* number of observation epochs */
static int iobsu = 0;            /* current rover observation data index */
static int iobsr = 0;            /* current reference observation data index */
static int isbs = 0;            /* current sbas message index */
static int ilex = 0;            /* current lex message index */
static int revs = 0;            /* analysis direction (0:forward,1:backward) */
static int aborts = 0;            /* abort status */
static sol_t *solf;             /* forward solutions */
static sol_t *solb;             /* backward solutions */
static double *rbf;             /* forward base positions */
static double *rbb;             /* backward base positions */
static int isolf = 0;             /* current forward solutions index */
static int isolb = 0;             /* current backward solutions index */
static char proc_rov[64] = "";   /* rover for current processing */
static char proc_base[64] = "";   /* base station for current processing */
static char rtcm_file[1024] = ""; /* rtcm data file */
static char rtcm_path[1024] = ""; /* rtcm data path */
static rtcm_t rtcm;             /* rtcm control struct */
static FILE *fp_rtcm = NULL;      /* rtcm data file pointer */

/* show message and check break ----------------------------------------------*/
static int checkbrk(const char *format, ...) {
	va_list arg;
	char buff[1024], *p = buff;
	if (!*format) return showmsg("");
	va_start(arg, format);
	p += vsprintf(p, format, arg);
	va_end(arg);
	if (*proc_rov&&*proc_base) sprintf(p, " (%s-%s)", proc_rov, proc_base);
	else if (*proc_rov) sprintf(p, " (%s)", proc_rov);
	else if (*proc_base) sprintf(p, " (%s)", proc_base);
	return showmsg(buff);
}
/* output reference position -------------------------------------------------*/
static void outrpos(FILE *fp, const double *r, const solopt_t *opt) {
	double pos[3], dms1[3], dms2[3];
	const char *sep = opt->sep;

	trace(3, "outrpos :\n");

	if (opt->posf == SOLF_LLH || opt->posf == SOLF_ENU) {
		ecef2pos(r, pos);
		if (opt->degf) {
			deg2dms(pos[0] * R2D, dms1);
			deg2dms(pos[1] * R2D, dms2);
			fprintf(fp, "%3.0f%s%02.0f%s%08.5f%s%4.0f%s%02.0f%s%08.5f%s%10.4f",
				dms1[0], sep, dms1[1], sep, dms1[2], sep, dms2[0], sep, dms2[1],
				sep, dms2[2], sep, pos[2]);
		} else {
			fprintf(fp, "%13.9f%s%14.9f%s%10.4f", pos[0] * R2D, sep, pos[1] * R2D,
				sep, pos[2]);
		}
	} else if (opt->posf == SOLF_XYZ) {
		fprintf(fp, "%14.4f%s%14.4f%s%14.4f", r[0], sep, r[1], sep, r[2]);
	}
}
/* output header -------------------------------------------------------------*/
static void outheader(FILE *fp, char **file, int n, const prcopt_t *popt,
	const solopt_t *sopt) {
	const char *s1[] = { "GPST", "UTC", "JST" };
	gtime_t ts, te;
	double t1, t2;
	int i, j, w1, w2;
	char s2[32], s3[32];

	trace(3, "outheader: n=%d\n", n);

	if (sopt->posf == SOLF_NMEA) return;

	if (sopt->outhead) {
		if (!*sopt->prog) {
			fprintf(fp, "%s program   : RTKLIB ver.%s\n", COMMENTH, VER_RTKLIB);
		} else {
			fprintf(fp, "%s program   : %s\n", COMMENTH, sopt->prog);
		}
		for (i = 0; i < n; i++) {
			fprintf(fp, "%s inp file  : %s\n", COMMENTH, file[i]);
		}
		for (i = 0; i < obss.n; i++)    if (obss.data[i].rcv == 1) break;
		for (j = obss.n - 1; j >= 0; j--) if (obss.data[j].rcv == 1) break;
		if (j < i) { fprintf(fp, "\n%s no rover obs data\n", COMMENTH); return; }
		ts = obss.data[i].time;
		te = obss.data[j].time;
		t1 = time2gpst(ts, &w1);
		t2 = time2gpst(te, &w2);
		if (sopt->times >= 1) ts = gpst2utc(ts);
		if (sopt->times >= 1) te = gpst2utc(te);
		if (sopt->times == 2) ts = timeadd(ts, 9 * 3600.0);
		if (sopt->times == 2) te = timeadd(te, 9 * 3600.0);
		time2str(ts, s2, 1);
		time2str(te, s3, 1);
		fprintf(fp, "%s obs start : %s %s (week%04d %8.1fs)\n", COMMENTH, s2, s1[sopt->times], w1, t1);
		fprintf(fp, "%s obs end   : %s %s (week%04d %8.1fs)\n", COMMENTH, s3, s1[sopt->times], w2, t2);
	}
	if (sopt->outopt) {
		outprcopt(fp, popt);
	}
	if (PMODE_DGPS <= popt->mode&&popt->mode <= PMODE_FIXED&&popt->mode != PMODE_MOVEB) {
		fprintf(fp, "%s ref pos   :", COMMENTH);
		outrpos(fp, popt->rb, sopt);
		fprintf(fp, "\n");
	}
	if (sopt->outhead || sopt->outopt) fprintf(fp, "%s\n", COMMENTH);

	outsolhead(fp, sopt);
}
/* search next observation data index ----------------------------------------*/
static int nextobsf(const obs_t *obs, int *i, int rcv) {
	double tt;
	int n;

	for (; *i < obs->n; (*i)++) if (obs->data[*i].rcv == rcv) break;
	for (n = 0; *i + n<obs->n; n++) {
		tt = timediff(obs->data[*i + n].time, obs->data[*i].time);
		if (obs->data[*i + n].rcv != rcv || tt>DTTOL) break;
	}
	return n;
}
static int nextobsb(const obs_t *obs, int *i, int rcv) {
	double tt;
	int n;

	for (; *i >= 0; (*i)--) if (obs->data[*i].rcv == rcv) break;
	for (n = 0; *i - n >= 0; n++) {
		tt = timediff(obs->data[*i - n].time, obs->data[*i].time);
		if (obs->data[*i - n].rcv != rcv || tt < -DTTOL) break;
	}
	return n;
}
/* input obs data, navigation messages and sbas correction -------------------*/
static int inputobs(obsd_t *obs, int solq, const prcopt_t *popt) {
	gtime_t time = { 0 };
	char path[1024];
	int i, nu, nr, n = 0;

	trace(3, "infunc  : revs=%d iobsu=%d iobsr=%d isbs=%d\n", revs, iobsu, iobsr, isbs);

	if (0 <= iobsu&&iobsu < obss.n) {
		settime((time = obss.data[iobsu].time));
		if (checkbrk("processing : %s Q=%d", time_str(time, 0), solq)) {
			aborts = 1; showmsg("aborted"); return -1;
		}
	}
	if (!revs) { /* input forward data */
		if ((nu = nextobsf(&obss, &iobsu, 1)) <= 0) return -1;
		if (popt->intpref) {
			for (; (nr = nextobsf(&obss, &iobsr, 2)) > 0; iobsr += nr)
			if (timediff(obss.data[iobsr].time, obss.data[iobsu].time) > -DTTOL) break;
		} else {
			for (i = iobsr; (nr = nextobsf(&obss, &i, 2)) > 0; iobsr = i, i += nr)
			if (timediff(obss.data[i].time, obss.data[iobsu].time) > DTTOL) break;
		}
		nr = nextobsf(&obss, &iobsr, 2);
		for (i = 0; i < nu&&n < MAXOBS * 2; i++) obs[n++] = obss.data[iobsu + i];
		for (i = 0; i < nr&&n < MAXOBS * 2; i++) obs[n++] = obss.data[iobsr + i];
		iobsu += nu;

		/* update sbas corrections */
		while (isbs<sbss.n) {
			time = gpst2time(sbss.msgs[isbs].week, sbss.msgs[isbs].tow);

			if (getbitu(sbss.msgs[isbs].msg, 8, 6) != 9) { /* except for geo nav */
				sbsupdatecorr(sbss.msgs + isbs, &navs);
			}
			if (timediff(time, obs[0].time)>-1.0 - DTTOL) break;
			isbs++;
		}
		/* update lex corrections */
		while (ilex<lexs.n) {
			if (lexupdatecorr(lexs.msgs + ilex, &navs, &time)) {
				if (timediff(time, obs[0].time)>-1.0 - DTTOL) break;
			}
			ilex++;
		}
		/* update rtcm corrections */
		if (*rtcm_file) {

			/* open or swap rtcm file */
			reppath(rtcm_file, path, obs[0].time, "", "");

			if (strcmp(path, rtcm_path)) {
				strcpy(rtcm_path, path);

				if (fp_rtcm) fclose(fp_rtcm);
				fp_rtcm = fopen(path, "rb");
				if (fp_rtcm) {
					rtcm.time = obs[0].time;
					input_rtcm3f(&rtcm, fp_rtcm);
					trace(2, "rtcm file open: %s\n", path);
				}
			}
			if (fp_rtcm) {
				while (timediff(rtcm.time, obs[0].time) < 0.0) {
					if (input_rtcm3f(&rtcm, fp_rtcm) < -1) break;
				}
				for (i = 0; i<MAXSAT; i++) navs.ssr[i] = rtcm.ssr[i];
			}
		}
	} else { /* input backward data */
		if ((nu = nextobsb(&obss, &iobsu, 1)) <= 0) return -1;
		if (popt->intpref) {
			for (; (nr = nextobsb(&obss, &iobsr, 2))>0; iobsr -= nr)
			if (timediff(obss.data[iobsr].time, obss.data[iobsu].time)<DTTOL) break;
		} else {
			for (i = iobsr; (nr = nextobsb(&obss, &i, 2))>0; iobsr = i, i -= nr)
			if (timediff(obss.data[i].time, obss.data[iobsu].time) < -DTTOL) break;
		}
		nr = nextobsb(&obss, &iobsr, 2);
		for (i = 0; i < nu&&n < MAXOBS * 2; i++) obs[n++] = obss.data[iobsu - nu + 1 + i];
		for (i = 0; i < nr&&n < MAXOBS * 2; i++) obs[n++] = obss.data[iobsr - nr + 1 + i];
		iobsu -= nu;

		/* update sbas corrections */
		while (isbs >= 0) {
			time = gpst2time(sbss.msgs[isbs].week, sbss.msgs[isbs].tow);

			if (getbitu(sbss.msgs[isbs].msg, 8, 6) != 9) { /* except for geo nav */
				sbsupdatecorr(sbss.msgs + isbs, &navs);
			}
			if (timediff(time, obs[0].time) < 1.0 + DTTOL) break;
			isbs--;
		}
		/* update lex corrections */
		while (ilex >= 0) {
			if (lexupdatecorr(lexs.msgs + ilex, &navs, &time)) {
				if (timediff(time, obs[0].time) < 1.0 + DTTOL) break;
			}
			ilex--;
		}
	}
	return n;
}
/* process positioning -------------------------------------------------------*/
static void procpos(FILE *fp, const prcopt_t *popt, const solopt_t *sopt,
	int mode) {
	gtime_t time = { 0 };
	sol_t sol = { { 0 } };
	rtk_t rtk;
	obsd_t obs[MAXOBS * 2]; /* for rover and base */
	double rb[3] = { 0 };
	int i, nobs, n, solstatic, pri[] = { 0, 1, 2, 3, 4, 5, 1, 6 };

	trace(3, "procpos : mode=%d\n", mode);

	solstatic = sopt->solstatic &&
		(popt->mode == PMODE_STATIC || popt->mode == PMODE_PPP_STATIC);

	rtkinit(&rtk, popt);
	rtcm_path[0] = '\0';

	while ((nobs = inputobs(obs, rtk.sol.stat, popt)) >= 0) {

		/* exclude satellites */
		for (i = n = 0; i < nobs; i++) {
			if ((satsys(obs[i].sat, NULL)&popt->navsys) &&
				popt->exsats[obs[i].sat - 1] != 1) obs[n++] = obs[i];
		}
		if (n <= 0) continue;

		if (!rtkpos(&rtk, obs, n, &navs)) continue;

		if (mode == 0) { /* forward/backward */
			if (!solstatic) {
				outsol(fp, &rtk.sol, rtk.rb, sopt);
			} else if (time.time == 0 || pri[rtk.sol.stat] <= pri[sol.stat]) {
				sol = rtk.sol;
				for (i = 0; i < 3; i++) rb[i] = rtk.rb[i];
				if (time.time == 0 || timediff(rtk.sol.time, time) < 0.0) {
					time = rtk.sol.time;
				}
			}
		} else if (!revs) { /* combined-forward */
			if (isolf >= nepoch) return;
			solf[isolf] = rtk.sol;
			for (i = 0; i < 3; i++) rbf[i + isolf * 3] = rtk.rb[i];
			isolf++;
		} else { /* combined-backward */
			if (isolb >= nepoch) return;
			solb[isolb] = rtk.sol;
			for (i = 0; i < 3; i++) rbb[i + isolb * 3] = rtk.rb[i];
			isolb++;
		}
	}
	if (mode == 0 && solstatic&&time.time != 0.0) {
		sol.time = time;
		outsol(fp, &sol, rb, sopt);
	}
	rtkfree(&rtk);
}
/* validation of combined solutions ------------------------------------------*/
static int valcomb(const sol_t *solf, const sol_t *solb) {
	double dr[3], var[3];
	int i;
	char tstr[32];

	trace(3, "valcomb :\n");

	/* compare forward and backward solution */
	for (i = 0; i < 3; i++) {
		dr[i] = solf->rr[i] - solb->rr[i];
		var[i] = solf->qr[i] + solb->qr[i];
	}
	for (i = 0; i < 3; i++) {
		if (dr[i] * dr[i] <= 16.0*var[i]) continue; /* ok if in 4-sigma */

		time2str(solf->time, tstr, 2);
		trace(2, "degrade fix to float: %s dr=%.3f %.3f %.3f std=%.3f %.3f %.3f\n",
			tstr + 11, dr[0], dr[1], dr[2], SQRT(var[0]), SQRT(var[1]), SQRT(var[2]));
		return 0;
	}
	return 1;
}
/* combine forward/backward solutions and output results ---------------------*/
static void combres(FILE *fp, const prcopt_t *popt, const solopt_t *sopt) {
	gtime_t time = { 0 };
	sol_t sols = { { 0 } }, sol = { { 0 } };
	double tt, Qf[9], Qb[9], Qs[9], rbs[3] = { 0 }, rb[3] = { 0 }, rr_f[3], rr_b[3], rr_s[3];
	int i, j, k, solstatic, pri[] = { 0, 1, 2, 3, 4, 5, 1, 6 };

	trace(3, "combres : isolf=%d isolb=%d\n", isolf, isolb);

	solstatic = sopt->solstatic &&
		(popt->mode == PMODE_STATIC || popt->mode == PMODE_PPP_STATIC);

	for (i = 0, j = isolb - 1; i < isolf&&j >= 0; i++, j--) {

		if ((tt = timediff(solf[i].time, solb[j].time)) < -DTTOL) {
			sols = solf[i];
			for (k = 0; k<3; k++) rbs[k] = rbf[k + i * 3];
			j++;
		} else if (tt>DTTOL) {
			sols = solb[j];
			for (k = 0; k < 3; k++) rbs[k] = rbb[k + j * 3];
			i--;
		} else if (solf[i].stat < solb[j].stat) {
			sols = solf[i];
			for (k = 0; k<3; k++) rbs[k] = rbf[k + i * 3];
		} else if (solf[i].stat>solb[j].stat) {
			sols = solb[j];
			for (k = 0; k < 3; k++) rbs[k] = rbb[k + j * 3];
		} else {
			sols = solf[i];
			sols.time = timeadd(sols.time, -tt / 2.0);

			if ((popt->mode == PMODE_KINEMA || popt->mode == PMODE_MOVEB) &&
				sols.stat == SOLQ_FIX) {

				/* degrade fix to float if validation failed */
				if (!valcomb(solf + i, solb + j)) sols.stat = SOLQ_FLOAT;
			}
			for (k = 0; k < 3; k++) {
				Qf[k + k * 3] = solf[i].qr[k];
				Qb[k + k * 3] = solb[j].qr[k];
			}
			Qf[1] = Qf[3] = solf[i].qr[3];
			Qf[5] = Qf[7] = solf[i].qr[4];
			Qf[2] = Qf[6] = solf[i].qr[5];
			Qb[1] = Qb[3] = solb[j].qr[3];
			Qb[5] = Qb[7] = solb[j].qr[4];
			Qb[2] = Qb[6] = solb[j].qr[5];

			if (popt->mode == PMODE_MOVEB) {
				for (k = 0; k < 3; k++) rr_f[k] = solf[i].rr[k] - rbf[k + i * 3];
				for (k = 0; k < 3; k++) rr_b[k] = solb[j].rr[k] - rbb[k + j * 3];
				if (smoother(rr_f, Qf, rr_b, Qb, 3, rr_s, Qs)) continue;
				for (k = 0; k < 3; k++) sols.rr[k] = rbs[k] + rr_s[k];
			} else {
				if (smoother(solf[i].rr, Qf, solb[j].rr, Qb, 3, sols.rr, Qs)) continue;
			}
			sols.qr[0] = (float)Qs[0];
			sols.qr[1] = (float)Qs[4];
			sols.qr[2] = (float)Qs[8];
			sols.qr[3] = (float)Qs[1];
			sols.qr[4] = (float)Qs[5];
			sols.qr[5] = (float)Qs[2];
		}
		if (!solstatic) {
			outsol(fp, &sols, rbs, sopt);
		} else if (time.time == 0 || pri[sols.stat] <= pri[sol.stat]) {
			sol = sols;
			for (k = 0; k < 3; k++) rb[k] = rbs[k];
			if (time.time == 0 || timediff(sols.time, time) < 0.0) {
				time = sols.time;
			}
		}
	}
	if (solstatic&&time.time != 0.0) {
		sol.time = time;
		outsol(fp, &sol, rb, sopt);
	}
}
/* read prec ephemeris, sbas data, lex data, tec grid and open rtcm ----------*/
static void readpreceph(char **infile, int n, const prcopt_t *prcopt,
	nav_t *nav, sbs_t *sbs, lex_t *lex) {
	seph_t seph0 = { 0 };
	int i;
	char *ext;

	trace(3, "readpreceph: n=%d\n", n);

	nav->ne = nav->nemax = 0;
	nav->nc = nav->ncmax = 0;
	sbs->n = sbs->nmax = 0;
	lex->n = lex->nmax = 0;

	/* read precise ephemeris files */
	for (i = 0; i < n; i++) {
		if (strstr(infile[i], "%r") || strstr(infile[i], "%b")) continue;
		readsp3(infile[i], nav, 0);
	}
	/* read precise clock files */
	for (i = 0; i < n; i++) {
		if (strstr(infile[i], "%r") || strstr(infile[i], "%b")) continue;
		readrnxc(infile[i], nav);
	}
	/* read sbas message files */
	for (i = 0; i < n; i++) {
		if (strstr(infile[i], "%r") || strstr(infile[i], "%b")) continue;
		sbsreadmsg(infile[i], prcopt->sbassatsel, sbs);
	}
	/* read lex message files */
	for (i = 0; i < n; i++) {
		if (strstr(infile[i], "%r") || strstr(infile[i], "%b")) continue;
		lexreadmsg(infile[i], 0, lex);
	}
	/* allocate sbas ephemeris */
	nav->ns = nav->nsmax = NSATSBS * 2;
	if (!(nav->seph = (seph_t *)malloc(sizeof(seph_t)*nav->ns))) {
		showmsg("error : sbas ephem memory allocation");
		trace(1, "error : sbas ephem memory allocation");
		return;
	}
	for (i = 0; i < nav->ns; i++) nav->seph[i] = seph0;

	/* set rtcm file and initialize rtcm struct */
	rtcm_file[0] = rtcm_path[0] = '\0'; fp_rtcm = NULL;

	for (i = 0; i < n; i++) {
		if ((ext = strrchr(infile[i], '.')) &&
			(!strcmp(ext, ".rtcm3") || !strcmp(ext, ".RTCM3"))) {
			strcpy(rtcm_file, infile[i]);
			init_rtcm(&rtcm);
			break;
		}
	}
}
/* free prec ephemeris and sbas data -----------------------------------------*/
static void freepreceph(nav_t *nav, sbs_t *sbs, lex_t *lex) {
	int i;

	trace(3, "freepreceph:\n");

	free(nav->peph); nav->peph = NULL; nav->ne = nav->nemax = 0;
	free(nav->pclk); nav->pclk = NULL; nav->nc = nav->ncmax = 0;
	free(nav->seph); nav->seph = NULL; nav->ns = nav->nsmax = 0;
	free(sbs->msgs); sbs->msgs = NULL; sbs->n = sbs->nmax = 0;
	free(lex->msgs); lex->msgs = NULL; lex->n = lex->nmax = 0;
	for (i = 0; i < nav->nt; i++) {
		free(nav->tec[i].data);
		free(nav->tec[i].rms);
	}
	free(nav->tec); nav->tec = NULL; nav->nt = nav->ntmax = 0;

#ifdef EXTSTEC
	stec_free(nav);
#endif

	if (fp_rtcm) fclose(fp_rtcm);
	free_rtcm(&rtcm);
}
/* read obs and nav data -----------------------------------------------------*/
static int readobsnav(gtime_t ts, gtime_t te, double ti, char **infile,
	const int *index, int n, const prcopt_t *prcopt,
	obs_t *obs, nav_t *nav, sta_t *sta) {
	int i, j, ind = 0, nobs = 0, rcv = 1;

	trace(3, "readobsnav: ts=%s n=%d\n", time_str(ts, 0), n);

	obs->data = NULL; obs->n = obs->nmax = 0;
	nav->eph = NULL; nav->n = nav->nmax = 0;
	nav->geph = NULL; nav->ng = nav->ngmax = 0;
	nav->seph = NULL; nav->ns = nav->nsmax = 0;
	nepoch = 0;

	for (i = 0; i<n; i++) {
		if (checkbrk("")) {
			return 0;
		}
		if (index[i] != ind) {
			if (obs->n>nobs) rcv++;
			ind = index[i]; nobs = obs->n;
		}
		/* read rinex obs and nav file */
		if (readrnxt(infile[i], rcv, ts, te, ti, prcopt->rnxopt[rcv <= 1 ? 0 : 1], obs, nav,
			rcv <= 2 ? sta + rcv - 1 : NULL) < 0) {
			checkbrk("error : insufficient memory");
			trace(1, "insufficient memory\n");
			return 0;
		}
	}
	if (obs->n <= 0) {
		checkbrk("error : no obs data");
		trace(1, "no obs data\n");
		return 0;
	}
	if (nav->n <= 0 && nav->ng <= 0 && nav->ns <= 0) {
		checkbrk("error : no nav data");
		trace(1, "no nav data\n");
		return 0;
	}
	/* sort observation data */
	nepoch = sortobs(obs);

	/* delete duplicated ephemeris */
	uniqnav(nav);

	/* set time span for progress display */
	if (ts.time == 0 || te.time == 0) {
		for (i = 0; i < obs->n; i++) if (obs->data[i].rcv == 1) break;
		for (j = obs->n - 1; j >= 0; j--) if (obs->data[j].rcv == 1) break;
		if (i < j) {
			if (ts.time == 0) ts = obs->data[i].time;
			if (te.time == 0) te = obs->data[j].time;
			settspan(ts, te);
		}
	}
	return 1;
}
/* free obs and nav data -----------------------------------------------------*/
static void freeobsnav(obs_t *obs, nav_t *nav) {
	trace(3, "freeobsnav:\n");

	free(obs->data); obs->data = NULL; obs->n = obs->nmax = 0;
	free(nav->eph); nav->eph = NULL; nav->n = nav->nmax = 0;
	free(nav->geph); nav->geph = NULL; nav->ng = nav->ngmax = 0;
	free(nav->seph); nav->seph = NULL; nav->ns = nav->nsmax = 0;
}
/* average of single position ------------------------------------------------*/
static int avepos(double *ra, int rcv, const obs_t *obs, const nav_t *nav,
	const prcopt_t *opt) {
	obsd_t data[MAXOBS];
	gtime_t ts = { 0 };
	sol_t sol = { { 0 } };
	int i, j, n = 0, m, iobs;
	char msg[128];

	trace(3, "avepos: rcv=%d obs.n=%d\n", rcv, obs->n);

	for (i = 0; i<3; i++) ra[i] = 0.0;

	for (iobs = 0; (m = nextobsf(obs, &iobs, rcv))>0; iobs += m) {

		for (i = j = 0; i < m&&i < MAXOBS; i++) {
			data[j] = obs->data[iobs + i];
			if ((satsys(data[j].sat, NULL)&opt->navsys) &&
				opt->exsats[data[j].sat - 1] != 1) j++;
		}
		if (j <= 0 || !screent(data[0].time, ts, ts, 1.0)) continue; /* only 1 hz */

		if (!pntpos(data, j, nav, opt, &sol, NULL, NULL, msg)) continue;

		for (i = 0; i < 3; i++) ra[i] += sol.rr[i];
		n++;
	}
	if (n <= 0) {
		trace(1, "no average of base station position\n");
		return 0;
	}
	for (i = 0; i < 3; i++) ra[i] /= n;
	return 1;
}
/* station position from file ------------------------------------------------*/
static int getstapos(const char *file, char *name, double *r) {
	FILE *fp;
	char buff[256], sname[256], *p, *q;
	double pos[3];

	trace(3, "getstapos: file=%s name=%s\n", file, name);

	if (!(fp = fopen(file, "r"))) {
		trace(1, "station position file open error: %s\n", file);
		return 0;
	}
	while (fgets(buff, sizeof(buff), fp)) {
		if ((p = strchr(buff, '%'))) *p = '\0';

		if (sscanf(buff, "%lf %lf %lf %s", pos, pos + 1, pos + 2, sname) < 4) continue;

		for (p = sname, q = name; *p&&*q; p++, q++) {
			if (toupper((int)*p) != toupper((int)*q)) break;
		}
		if (!*p) {
			pos[0] *= D2R;
			pos[1] *= D2R;
			pos2ecef(pos, r);
			fclose(fp);
			return 1;
		}
	}
	fclose(fp);
	trace(1, "no station position: %s %s\n", name, file);
	return 0;
}
/* antenna phase center position ---------------------------------------------*/
static int antpos(prcopt_t *opt, int rcvno, const obs_t *obs, const nav_t *nav,
	const sta_t *sta, const char *posfile) {
	double *rr = rcvno == 1 ? opt->ru : opt->rb, del[3], pos[3], dr[3] = { 0 };
	int i, postype = rcvno == 1 ? opt->rovpos : opt->refpos;
	char *name;

	trace(3, "antpos  : rcvno=%d\n", rcvno);

	if (postype == 1) { /* average of single position */
		if (!avepos(rr, rcvno, obs, nav, opt)) {
			showmsg("error : station pos computation");
			return 0;
		}
	} else if (postype == 2) { /* read from position file */
		name = stas[rcvno == 1 ? 0 : 1].name;
		if (!getstapos(posfile, name, rr)) {
			showmsg("error : no position of %s in %s", name, posfile);
			return 0;
		}
	} else if (postype == 3) { /* get from rinex header */
		if (norm(stas[rcvno == 1 ? 0 : 1].pos, 3) <= 0.0) {
			showmsg("error : no position in rinex header");
			trace(1, "no position position in rinex header\n");
			return 0;
		}
		/* antenna delta */
		if (stas[rcvno == 1 ? 0 : 1].deltype == 0) { /* enu */
			for (i = 0; i < 3; i++) del[i] = stas[rcvno == 1 ? 0 : 1].del[i];
			del[2] += stas[rcvno == 1 ? 0 : 1].hgt;
			ecef2pos(stas[rcvno == 1 ? 0 : 1].pos, pos);
			enu2ecef(pos, del, dr);
		} else { /* xyz */
			for (i = 0; i < 3; i++) dr[i] = stas[rcvno == 1 ? 0 : 1].del[i];
		}
		for (i = 0; i < 3; i++) rr[i] = stas[rcvno == 1 ? 0 : 1].pos[i] + dr[i];
	}
	return 1;
}
/* open procssing session ----------------------------------------------------*/
static int openses(const prcopt_t *popt, const solopt_t *sopt,
	const filopt_t *fopt, nav_t *nav, pcvs_t *pcvs, pcvs_t *pcvr) {
	char *ext;

	trace(3, "openses :\n");

	/* read satellite antenna parameters */
	if (*fopt->satantp&&!(readpcv(fopt->satantp, pcvs))) {
		showmsg("error : no sat ant pcv in %s", fopt->satantp);
		trace(1, "sat antenna pcv read error: %s\n", fopt->satantp);
		return 0;
	}
	/* read receiver antenna parameters */
	if (*fopt->rcvantp&&!(readpcv(fopt->rcvantp, pcvr))) {
		showmsg("error : no rec ant pcv in %s", fopt->rcvantp);
		trace(1, "rec antenna pcv read error: %s\n", fopt->rcvantp);
		return 0;
	}
	/* read dcb parameters */
	if (*fopt->dcb) {
		readdcb(fopt->dcb, nav);
	}
	/* read ionosphere data file */
	if (*fopt->iono && (ext = strrchr(fopt->iono, '.'))) {
		if (strlen(ext) == 4 && (ext[3] == 'i' || ext[3] == 'I')) {
			readtec(fopt->iono, nav, 0);
		}
#ifdef EXTSTEC
		else if (!strcmp(ext,".stec")||!strcmp(ext,".STEC")) {
			stec_read(fopt->iono,nav);
		}
#endif
	}
	/* open geoid data */
	if (sopt->geoid > 0 && *fopt->geoid) {
		if (!opengeoid(sopt->geoid, fopt->geoid)) {
			showmsg("error : no geoid data %s", fopt->geoid);
			trace(2, "no geoid data %s\n", fopt->geoid);
		}
	}
	/* read erp data */
	if (*fopt->eop) {
		if (!readerp(fopt->eop, &nav->erp)) {
			showmsg("error : no erp data %s", fopt->eop);
			trace(2, "no erp data %s\n", fopt->eop);
		}
	}
	return 1;
}
/* close procssing session ---------------------------------------------------*/
static void closeses(nav_t *nav, pcvs_t *pcvs, pcvs_t *pcvr) {
	trace(3, "closeses:\n");

	/* free antenna parameters */
	free(pcvs->pcv); pcvs->pcv = NULL; pcvs->n = pcvs->nmax = 0;
	free(pcvr->pcv); pcvr->pcv = NULL; pcvr->n = pcvr->nmax = 0;

	/* close geoid data */
	closegeoid();

	/* free erp data */
	free(nav->erp.data); nav->erp.data = NULL; nav->erp.n = nav->erp.nmax = 0;

	/* close solution statistics and debug trace */
	rtkclosestat();
	traceclose();
}
/* set antenna parameters ----------------------------------------------------*/
static void setpcv(gtime_t time, prcopt_t *popt, nav_t *nav, const pcvs_t *pcvs,
	const pcvs_t *pcvr, const sta_t *sta) {
	pcv_t *pcv;
	double pos[3], del[3];
	int i, j, mode = PMODE_DGPS <= popt->mode&&popt->mode <= PMODE_FIXED;
	char id[64];

	/* set satellite antenna parameters */
	for (i = 0; i < MAXSAT; i++) {
		if (!(satsys(i + 1, NULL)&popt->navsys)) continue;
		if (!(pcv = searchpcv(i + 1, "", time, pcvs))) {
			satno2id(i + 1, id);
			trace(2, "no satellite antenna pcv: %s\n", id);
			continue;
		}
		nav->pcvs[i] = *pcv;
	}
	for (i = 0; i<(mode ? 2 : 1); i++) {
		if (!strcmp(popt->anttype[i], "*")) { /* set by station parameters */
			strcpy(popt->anttype[i], sta[i].antdes);
			if (sta[i].deltype == 1) { /* xyz */
				if (norm(sta[i].pos, 3)>0.0) {
					ecef2pos(sta[i].pos, pos);
					ecef2enu(pos, sta[i].del, del);
					for (j = 0; j < 3; j++) popt->antdel[i][j] = del[j];
				}
			} else { /* enu */
				for (j = 0; j < 3; j++) popt->antdel[i][j] = stas[i].del[j];
			}
		}
		if (!(pcv = searchpcv(0, popt->anttype[i], time, pcvr))) {
			trace(2, "no receiver antenna pcv: %s\n", popt->anttype[i]);
			*popt->anttype[i] = '\0';
			continue;
		}
		strcpy(popt->anttype[i], pcv->type);
		popt->pcvr[i] = *pcv;
	}
}
/* read ocean tide loading parameters ----------------------------------------*/
static void readotl(prcopt_t *popt, const char *file, const sta_t *sta) {
	int i, mode = PMODE_DGPS <= popt->mode&&popt->mode <= PMODE_FIXED;

	for (i = 0; i < (mode ? 2 : 1); i++) {
		readblq(file, sta[i].name, popt->odisp[i]);
	}
}
/* write header to output file -----------------------------------------------*/
static int outhead(const char *outfile, char **infile, int n,
	const prcopt_t *popt, const solopt_t *sopt) {
	FILE *fp = stdout;

	trace(3, "outhead: outfile=%s n=%d\n", outfile, n);

	if (*outfile) {
		createdir(outfile);

		if (!(fp = fopen(outfile, "w"))) {
			showmsg("error : open output file %s", outfile);
			return 0;
		}
	}
	/* output header */
	outheader(fp, infile, n, popt, sopt);

	if (*outfile) fclose(fp);

	return 1;
}
/* open output file for append -----------------------------------------------*/
static FILE *openfile(const char *outfile) {
	trace(3, "openfile: outfile=%s\n", outfile);

	return !*outfile ? stdout : fopen(outfile, "a");
}
/* execute processing session ------------------------------------------------*/
static int execses(gtime_t ts, gtime_t te, double ti, const prcopt_t *popt,
	const solopt_t *sopt, const filopt_t *fopt, int flag,
	char **infile, const int *index, int n, char *outfile) {
	FILE *fp;
	prcopt_t popt_ = *popt;
	char tracefile[1024], statfile[1024];

	trace(3, "execses : n=%d outfile=%s\n", n, outfile);

	/* open debug trace */
	if (flag&&sopt->trace > 0) {
		if (*outfile) {
			strcpy(tracefile, outfile);
			strcat(tracefile, ".trace");
		} else {
			strcpy(tracefile, fopt->trace);
		}
		traceclose();
		traceopen(tracefile);
		tracelevel(sopt->trace);
	}
	/* read obs and nav data */
	if (!readobsnav(ts, te, ti, infile, index, n, &popt_, &obss, &navs, stas)) {
		return 0;
	}

	/* set antenna paramters */
	if (popt_.mode != PMODE_SINGLE) {
		setpcv(obss.n > 0 ? obss.data[0].time : timeget(), &popt_, &navs, &pcvss, &pcvsr,
			stas);
	}
	/* read ocean tide loading parameters */
	if (popt_.mode > PMODE_SINGLE&&fopt->blq) {
		readotl(&popt_, fopt->blq, stas);
	}
	/* rover/reference fixed position */
	if (popt_.mode == PMODE_FIXED) {
		if (!antpos(&popt_, 1, &obss, &navs, stas, fopt->stapos)) {
			freeobsnav(&obss, &navs);
			return 0;
		}
	} else if (PMODE_DGPS <= popt_.mode&&popt_.mode <= PMODE_STATIC) {
		if (!antpos(&popt_, 2, &obss, &navs, stas, fopt->stapos)) {
			freeobsnav(&obss, &navs);
			return 0;
		}
	}
	/* open solution statistics */
	if (flag&&sopt->sstat > 0) {
		strcpy(statfile, outfile);
		strcat(statfile, ".stat");
		rtkclosestat();
		rtkopenstat(statfile, sopt->sstat);
	}
	/* write header to output file */
	if (flag&&!outhead(outfile, infile, n, &popt_, sopt)) {
		freeobsnav(&obss, &navs);
		return 0;
	}
	iobsu = iobsr = isbs = ilex = revs = aborts = 0;

	if (popt_.mode == PMODE_SINGLE || popt_.soltype == 0) {
		if ((fp = openfile(outfile))) {
			procpos(fp, &popt_, sopt, 0); /* forward */
			fclose(fp);
		}
	} else if (popt_.soltype == 1) {
		if ((fp = openfile(outfile))) {
			revs = 1; iobsu = iobsr = obss.n - 1; isbs = sbss.n - 1; ilex = lexs.n - 1;
			procpos(fp, &popt_, sopt, 0); /* backward */
			fclose(fp);
		}
	} else { /* combined */
		solf = (sol_t *)malloc(sizeof(sol_t)*nepoch);
		solb = (sol_t *)malloc(sizeof(sol_t)*nepoch);
		rbf = (double *)malloc(sizeof(double)*nepoch * 3);
		rbb = (double *)malloc(sizeof(double)*nepoch * 3);

		if (solf&&solb) {
			isolf = isolb = 0;
			procpos(NULL, &popt_, sopt, 1); /* forward */
			revs = 1; iobsu = iobsr = obss.n - 1; isbs = sbss.n - 1; ilex = lexs.n - 1;
			procpos(NULL, &popt_, sopt, 1); /* backward */

			/* combine forward/backward solutions */
			if (!aborts && (fp = openfile(outfile))) {
				combres(fp, &popt_, sopt);
				fclose(fp);
			}
		} else showmsg("error : memory allocation");
		free(solf);
		free(solb);
		free(rbf);
		free(rbb);
	}
	/* free obs and nav data */
	freeobsnav(&obss, &navs);

	return aborts ? 1 : 0;
}
/* execute processing session for each rover ---------------------------------*/
static int execses_r(gtime_t ts, gtime_t te, double ti, const prcopt_t *popt,
	const solopt_t *sopt, const filopt_t *fopt, int flag,
	char **infile, const int *index, int n, char *outfile,
	const char *rov) {
	gtime_t t0 = { 0 };
	int i, stat = 0;
	char *ifile[MAXINFILE], ofile[1024], *rov_, *p, *q, s[64] = "";

	trace(3, "execses_r: n=%d outfile=%s\n", n, outfile);

	for (i = 0; i < n; i++) if (strstr(infile[i], "%r")) break;

	if (i < n) { /* include rover keywords */
		if (!(rov_ = (char *)malloc(strlen(rov) + 1))) return 0;
		strcpy(rov_, rov);

		for (i = 0; i < n; i++) {
			if (!(ifile[i] = (char *)malloc(1024))) {
				free(rov_); for (; i >= 0; i--) free(ifile[i]);
				return 0;
			}
		}
		for (p = rov_;; p = q + 1) { /* for each rover */
			if ((q = strchr(p, ' '))) *q = '\0';

			if (*p) {
				strcpy(proc_rov, p);
				if (ts.time) time2str(ts, s, 0); else *s = '\0';
				if (checkbrk("reading    : %s", s)) {
					stat = 1;
					break;
				}
				for (i = 0; i < n; i++) reppath(infile[i], ifile[i], t0, p, "");
				reppath(outfile, ofile, t0, p, "");

				/* execute processing session */
				stat = execses(ts, te, ti, popt, sopt, fopt, flag, ifile, index, n, ofile);
			}
			if (stat == 1 || !q) break;
		}
		free(rov_); for (i = 0; i < n; i++) free(ifile[i]);
	} else {
		/* execute processing session */
		stat = execses(ts, te, ti, popt, sopt, fopt, flag, infile, index, n, outfile);
	}
	return stat;
}
/* execute processing session for each base station --------------------------*/
static int execses_b(gtime_t ts, gtime_t te, double ti, const prcopt_t *popt,
	const solopt_t *sopt, const filopt_t *fopt, int flag,
	char **infile, const int *index, int n, char *outfile,
	const char *rov, const char *base) {
	gtime_t t0 = { 0 };
	int i, stat = 0;
	char *ifile[MAXINFILE], ofile[1024], *base_, *p, *q, s[64];

	trace(3, "execses_b: n=%d outfile=%s\n", n, outfile);

	/* read prec ephemeris and sbas data */
	readpreceph(infile, n, popt, &navs, &sbss, &lexs);

	for (i = 0; i < n; i++) if (strstr(infile[i], "%b")) break;

	if (i < n) { /* include base station keywords */
		if (!(base_ = (char *)malloc(strlen(base) + 1))) {
			freepreceph(&navs, &sbss, &lexs);
			return 0;
		}
		strcpy(base_, base);

		for (i = 0; i < n; i++) {
			if (!(ifile[i] = (char *)malloc(1024))) {
				free(base_); for (; i >= 0; i--) free(ifile[i]);
				freepreceph(&navs, &sbss, &lexs);
				return 0;
			}
		}
		for (p = base_;; p = q + 1) { /* for each base station */
			if ((q = strchr(p, ' '))) *q = '\0';

			if (*p) {
				strcpy(proc_base, p);
				if (ts.time) time2str(ts, s, 0); else *s = '\0';
				if (checkbrk("reading    : %s", s)) {
					stat = 1;
					break;
				}
				for (i = 0; i < n; i++) reppath(infile[i], ifile[i], t0, "", p);
				reppath(outfile, ofile, t0, "", p);

				stat = execses_r(ts, te, ti, popt, sopt, fopt, flag, ifile, index, n, ofile, rov);
			}
			if (stat == 1 || !q) break;
		}
		free(base_); for (i = 0; i < n; i++) free(ifile[i]);
	} else {
		stat = execses_r(ts, te, ti, popt, sopt, fopt, flag, infile, index, n, outfile, rov);
	}
	/* free prec ephemeris and sbas data */
	freepreceph(&navs, &sbss, &lexs);

	return stat;
}
/* post-processing positioning -------------------------------------------------
* post-processing positioning
* args   : gtime_t ts       I   processing start time (ts.time==0: no limit)
*        : gtime_t te       I   processing end time   (te.time==0: no limit)
*          double ti        I   processing interval  (s) (0:all)
*          double tu        I   processing unit time (s) (0:all)
*          prcopt_t *popt   I   processing options
*          solopt_t *sopt   I   solution options
*          filopt_t *fopt   I   file options
*          char   **infile  I   input files (see below)
*          int    n         I   number of input files
*          char   *outfile  I   output file ("":stdout, see below)
*          char   *rov      I   rover id list        (separated by " ")
*          char   *base     I   base station id list (separated by " ")
* return : status (0:ok,0>:error,1:aborted)
* notes  : input files should contain observation data, navigation data, precise
*          ephemeris/clock (optional), sbas log file (optional), ssr message
*          log file (optional) and tec grid file (optional). only the first
*          observation data file in the input files is recognized as the rover
*          data.
*
*          the type of an input file is recognized by the file extention as ]
*          follows:
*              .sp3,.SP3,.eph*,.EPH*: precise ephemeris (sp3c)
*              .sbs,.SBS,.ems,.EMS  : sbas message log files (rtklib or ems)
*              .lex,.LEX            : qzss lex message log files
*              .rtcm3,.RTCM3        : ssr message log files (rtcm3)
*              .*i,.*I              : tec grid files (ionex)
*              others               : rinex obs, nav, gnav, hnav, qnav or clock
*
*          inputs files can include wild-cards (*). if an file includes
*          wild-cards, the wild-card expanded multiple files are used.
*
*          inputs files can include keywords. if an file includes keywords,
*          the keywords are replaced by date, time, rover id and base station
*          id and multiple session analyses run. refer reppath() for the
*          keywords.
*
*          the output file can also include keywords. if the output file does
*          not include keywords. the results of all multiple session analyses
*          are output to a single output file.
*
*          ssr corrections are valid only for forward estimation.
*-----------------------------------------------------------------------------*/
extern int postpos(gtime_t ts, gtime_t te, double ti, double tu,
	const prcopt_t *popt, const solopt_t *sopt,
	const filopt_t *fopt, char **infile, int n, char *outfile,
	const char *rov, const char *base) {
	gtime_t tts, tte, ttte;
	double tunit, tss;
	int i, j, k, nf, stat = 0, week, flag = 1, index[MAXINFILE] = { 0 };
	char *ifile[MAXINFILE], ofile[1024], *ext;

	trace(3, "postpos : ti=%.0f tu=%.0f n=%d outfile=%s\n", ti, tu, n, outfile);

	/* open processing session */
	if (!openses(popt, sopt, fopt, &navs, &pcvss, &pcvsr)) return -1;

	if (ts.time != 0 && te.time != 0 && tu >= 0.0) {
		if (timediff(te, ts) < 0.0) {
			showmsg("error : no period");
			closeses(&navs, &pcvss, &pcvsr);
			return 0;
		}
		for (i = 0; i<MAXINFILE; i++) {
			if (!(ifile[i] = (char *)malloc(1024))) {
				for (; i >= 0; i--) free(ifile[i]);
				closeses(&navs, &pcvss, &pcvsr);
				return -1;
			}
		}
		if (tu == 0.0 || tu>86400.0*MAXPRCDAYS) tu = 86400.0*MAXPRCDAYS;
		settspan(ts, te);
		tunit = tu<86400.0 ? tu : 86400.0;
		tss = tunit*(int)floor(time2gpst(ts, &week) / tunit);

		for (i = 0;; i++) { /* for each periods */
			tts = gpst2time(week, tss + i*tu);
			tte = timeadd(tts, tu - DTTOL);
			if (timediff(tts, te)>0.0) break;
			if (timediff(tts, ts)<0.0) tts = ts;
			if (timediff(tte, te)>0.0) tte = te;

			strcpy(proc_rov, "");
			strcpy(proc_base, "");
			if (checkbrk("reading    : %s", time_str(tts, 0))) {
				stat = 1;
				break;
			}
			for (j = k = nf = 0; j < n; j++) {

				ext = strrchr(infile[j], '.');

				if (ext && (!strcmp(ext, ".rtcm3") || !strcmp(ext, ".RTCM3"))) {
					strcpy(ifile[nf++], infile[j]);
				} else {
					/* include next day precise ephemeris or rinex brdc nav */
					ttte = tte;
					if (ext && (!strcmp(ext, ".sp3") || !strcmp(ext, ".SP3") ||
						!strcmp(ext, ".eph") || !strcmp(ext, ".EPH"))) {
						ttte = timeadd(ttte, 3600.0);
					} else if (strstr(infile[j], "brdc")) {
						ttte = timeadd(ttte, 7200.0);
					}
					nf += reppaths(infile[j], ifile + nf, MAXINFILE - nf, tts, ttte, "", "");
				}
				while (k<nf) index[k++] = j;

				if (nf >= MAXINFILE) {
					trace(2, "too many input files. trancated\n");
					break;
				}
			}
			if (!reppath(outfile, ofile, tts, "", "") && i>0) flag = 0;

			/* execute processing session */
			stat = execses_b(tts, tte, ti, popt, sopt, fopt, flag, ifile, index, nf, ofile,
				rov, base);

			if (stat == 1) break;
		}
		for (i = 0; i < MAXINFILE; i++) free(ifile[i]);
	} else if (ts.time != 0) {
		for (i = 0; i < n&&i < MAXINFILE; i++) {
			if (!(ifile[i] = (char *)malloc(1024))) {
				for (; i >= 0; i--) free(ifile[i]); return -1;
			}
			reppath(infile[i], ifile[i], ts, "", "");
			index[i] = i;
		}
		reppath(outfile, ofile, ts, "", "");

		/* execute processing session */
		stat = execses_b(ts, te, ti, popt, sopt, fopt, 1, ifile, index, n, ofile, rov,
			base);

		for (i = 0; i < n&&i < MAXINFILE; i++) free(ifile[i]);
	} else {
		for (i = 0; i < n; i++) index[i] = i;

		/* execute processing session */
		stat = execses_b(ts, te, ti, popt, sopt, fopt, 1, infile, index, n, outfile, rov,
			base);
	}
	/* close processing session */
	closeses(&navs, &pcvss, &pcvsr);

	return stat;
}






/*�Զ��庯��*/
void myxyz2blh(double* xyz, double* blh, double alfa, double RE) {
	double e2 = alfa*(2.0 - alfa), z, zk, v = RE, sinp;
	double r2 = xyz[0] * xyz[0] + xyz[1] * xyz[1];
	for (z = xyz[2], zk = 0.0; fabs(z - zk) >= 1E-4;) {
		zk = z;
		sinp = z / sqrt(r2 + z*z);
		v = RE / sqrt(1.0 - e2*sinp*sinp);
		z = xyz[2] + v*e2*sinp;
	}
	blh[0] = r2 > 1E-12 ? atan(z / sqrt(r2)) : (xyz[2] > 0.0 ? PI / 2.0 : -PI / 2.0);
	blh[1] = r2 > 1E-12 ? atan2(xyz[1], xyz[0]) : 0.0;
	blh[2] = sqrt(r2 + z*z) - v;

}
/**/
void myxyz2neu(double* staxyz, double* xyz, double* neu, double alfa, double RE) {
	double tmp[3] = { 0.0 };
	double blh[3] = { 0.0 };
	double m[9] = { 0.0 };//ת������
	double sinl = 0.0, cosl = 0.0, cosb = 0.0, sinb = 0.0;
	int i = 0;
	for (i = 0; i < 3; i++) {
		tmp[i] = xyz[i] - staxyz[i];
	}

	myxyz2blh(staxyz, blh, alfa, RE);
	sinl = sin(blh[1]);
	cosl = cos(blh[1]);
	cosb = cos(blh[0]);
	sinb = sin(blh[0]);

	m[0] = -sinb*cosl; m[1] = -sinb*sinl; m[2] = cosb;   //N
	m[3] = -sinl;     m[4] = cosl;      m[5] = 0.0;    //E
	m[6] = cosb*cosl; m[7] = cosb*sinl; m[8] = sinb;   //U

	neu[0] = m[0] * tmp[0] + m[1] * tmp[1] + m[2] * tmp[2];
	neu[1] = m[3] * tmp[0] + m[4] * tmp[1] + m[5] * tmp[2];
	neu[2] = m[6] * tmp[0] + m[7] * tmp[1] + m[8] * tmp[2];
}
/*
*purpose: get data(observation and broadcast ephemeris ) from rtcm and solve it
*
*
*/
#define MAXSTR      5  

extern int myRTpos( /*const prcopt_t *popt, const solopt_t *sopt,*/ char* ntrip_str) {
	////[user[:passwd]]@address[:port][/mountpoint]
	FILE* recordfile = NULL, *logfile = NULL;
	char satstr[3] = { 0 };
	int sys = -1, prn = -1, solstatic = -1, mode = 0, pri[] = { 0, 1, 2, 3, 4, 5, 1, 6 };
	gtime_t  mytime[32] = { { 0 } };
	gtime_t  time = { 0 };
	char timestr[20] = { 0 };
	int tag = 0;
	int intrflg = 1;
	obsd_t obs[MAXOBS];
	rtk_t rtk;
	prcopt_t myprcopt;
	solopt_t mysolopt;
	sol_t sol = { { 0 } };
	double rb[3] = { 0.0 }, pos[3] = { 0.0 }, e[3] = { 0.0 };
	strsvr_t  mysvr_obs, mysvr_ssr;
	int  dispint = 1000;
	//int opts[] = { 20000, 30000, 2000, 32768, 10, 0, 30 };
	int opts[] = { 10000, 10000, 2000, 32768, 10, 0, 30 };
	double stapos[3] = { 0 };
	//char*  myaddress = "yangmf:123456@59.175.223.165:2101/WHU01";  gxwang:wang123@58.49.58.149:2101/JFNG7
	char *paths[MAXSTR] = { "station:123456|59.175.223.165:2101/NTSCR9" };  //Get observation and navigation data from xian
	int types[MAXSTR] = { STR_NTRIPCLI, STR_FILE };
	strconv_t *myconv[MAXSTR] = { NULL };
	strconv_t *myconv_ssr[MAXSTR] = { NULL };
	char *msgout = "1004,1003,1238,1078,1060,1066";
	char* opt = "";
	int strstat[MAXSTR] = { 0 };
	int strstat_ssr[MAXSTR] = { 0 };
	int strbyte[MAXSTR] = { 0 };
	int strbyte_ssr[MAXSTR] = { 0 };
	int bps[MAXSTR] = { 0 };
	int bps_ssr[MAXSTR] = { 0 };
	char strmsg[MAXSTRMSG] = "";
	char strmsg_ssr[MAXSTRMSG] = "";
	const char ss[] = { 'E', '-', 'W', 'C', 'C' };
	char buff[256], *p;
	int i = 0, j = 0, n = 1, t1;  // n��ֵ������1
	int nobs = 0;
	int nepoch = 0;
	int test_svr_obs = 0, test_svr_ssr = 0;
	if (ntrip_str == NULL) {
		ntrip_str = "001@whu:123|119.97.244.11:2101/GK03";  //Get orbit & clock correction data from GK03
	} //001@whu:123|119.97.244.11:2101/WHU01
	paths[0] = ntrip_str;
	//paths[0] = "gxwang:wang123@58.49.58.149:2101/CLK20";
	paths[1] = "mytest.dat";
	myprcopt = prcopt_default;
	mysolopt = solopt_default;
	recordfile = fopen("log/ssc.log", "w+");
	logfile = fopen("log/ppp_ssc.log", "w+");
	fflush(recordfile);  //ȫ����	

	//myconv[0] = strconvnew(STRFMT_UB380, STRFMT_RTCM3, msgout, 0, 0, opt);
	myconv[0] = strconvnew(STRFMT_BINEX, STRFMT_RTCM3, msgout, 0, 0, opt);
	myconv_ssr[0] = strconvnew(STRFMT_RTCM3, STRFMT_RTCM3, msgout, 0, 0, opt);

	strsvrinit(&mysvr_obs, n);  //���ڽ���rtcm�۲����ݵ�server
	strsvrinit(&mysvr_ssr, n);  //���ڽ���ssr��������server
	//���������߳�
	test_svr_obs = strsvrstart(&mysvr_obs, opts, types, paths, myconv, NULL, stapos);
	if (!test_svr_obs) {
		printf("stream(RTCM) server start error\n");
		return -1;
	}

	paths[0] = "gxwang:wang123|58.49.58.149:2101/CLK91"; //Get orbit & clock correction data from IGS
	test_svr_ssr = strsvrstart(&mysvr_ssr, opts, types, paths, myconv_ssr, NULL, stapos);
	if (!test_svr_ssr) {
		printf("stream(RTCM) server start error\n");
		return -1;
	}

	myprcopt.navsys = SYS_GPS;
	myprcopt.tidecorr = 2;  // �������峱ϫ�����Ƹ���,����ϫ��
	myprcopt.elmin = 5.0*D2R;
	myprcopt.posopt[0] = 1; myprcopt.posopt[1] = 1; myprcopt.posopt[2] = 1; 
	myprcopt.posopt[3] = 1; myprcopt.posopt[4] = 1; myprcopt.posopt[5] = 1;
	myprcopt.ionoopt = IONOOPT_IFLC;
	myprcopt.tropopt = TROPOPT_EST; //����ZTD
	//myprcopt.tropopt = TROPOPT_SAAS ; //����ZTD

	myprcopt.mode = PMODE_PPP_KINEMA;
	//myprcopt.mode = PMODE_SINGLE;
	myprcopt.modear = 4;  //4:ppp-ar
	myprcopt.sateph = EPHOPT_SSRAPC;  //�����apc��com������������������λ�����Ƿ���и���
	myprcopt.nf = 1;  //���õ�Ƶ

	//myprcopt.navsys = SYS_GPS;
	//myprcopt.nf = 1;  //���õ�Ƶ
	//myprcopt.sateph = EPHOPT_BRDC;
	//myprcopt.elmin = 5.0*D2R;
	//myprcopt.mode = PMODE_SINGLE;

	mysolopt.datum = 0;
	mysolopt.posf = SOLF_ENU;
	//solopt.prog = prog;
	mysolopt.times = TIMES_GPST;
	mysolopt.sstat = 0;

	//��������
	//myprcopt.exsats[4] = 1;  //�ų���17������
	//myprcopt.exsats[9] = 1;  //�ų���13������

	solstatic = mysolopt.solstatic && (myprcopt.mode == PMODE_STATIC || myprcopt.mode == PMODE_PPP_STATIC);

	rtkinit(&rtk, &myprcopt);
	//myrtkinit(&rtk,&myprcopt);

	//ecef2pos(rtk.rb,pos);
	//��ʼѭ������
	for (intrflg = 0; !intrflg;) {
		nobs = 0;
		nepoch = 0;

		//strlock(mysvr_obs.stream);
		lock(&mysvr_obs.lock);
		navs = myconv[0]->out.nav;  //update the static variable

		if (myconv[0]->out.obsflag == 1) {  // obs data complete flag 1:ok
			obss = myconv[0]->out.obs;  // update static variable
			//obss = myconv[0]->raw.obs;  // update static variable

			for (i = 0; i < obss.n; i++) {
				obss.data[i].rcv = 1;  // 1 ��ʾ��׼վ
			}
			nepoch = sortobs(&obss); //sort and unique observation data by time, rcv, sat
			time2str(obss.data->time, timestr, 3);
		}

		unlock(&mysvr_obs.lock);

		//����ssr�������߳�
		lock(&mysvr_ssr.lock);

		memcpy(navs.ssr, myconv_ssr[0]->rtcm.ssr, sizeof(ssr_t)*MAXSAT);

		unlock(&mysvr_ssr.lock);

		strsvrstat(&mysvr_obs, strstat, strbyte, bps, strmsg);

		strsvrstat(&mysvr_ssr, strstat_ssr, strbyte_ssr, bps_ssr, strmsg_ssr);

		t1 = tickget();

		/* update carrier wave length */
		for (i = 0; i < MAXSAT; i++) {
			for (j = 0; j < NFREQ; j++) {
				navs.lam[i][j] = satwavelen(i + 1, j, &navs);
			}
		}
		printf("������Ϣ:%d\n", myconv[0]->out.ephsat);
		for (i = 0; i < navs.n; i++) {
			printf("%d  ", navs.eph[i].sat);
		}
		for (i = 0; i < navs.n; i++) {
			sys = satsys(navs.eph[i].sat, &prn);
			if (sys != SYS_GPS) {
				continue;
			}
			satno2id(navs.eph[i].sat, satstr);
			printf("%s:%.4f ", satstr, navs.eph[i].A);
		}
		printf("\n");

		//���õ�ǰ��ʱ��
		if (myconv[0]->out.obsflag != 0) {
			printf("�۲�ֵ��Ϣ:%d\n", myconv[0]->out.obsflag);
			printf("epochtime:%s             \n", timestr);
			for (i = 0; i < obss.n; i++) {
				sys = satsys(obss.data[i].sat, &prn);
				if (sys != SYS_GPS) {
					continue;
				}
				satno2id(obss.data[i].sat, satstr);
				printf("sat: %s obs:%.3f  %.3f %.3f %.3f \n", satstr,
					obss.data[i].P[0], obss.data[i].P[1],
					obss.data[i].L[0], obss.data[i].L[1]);
			}
			printf("\n");
		}

		nobs = inputobs(obs, rtk.sol.stat, &myprcopt);
		if (nobs > 0) {
			/* exclude satellites */
			for (i = n = 0; i < nobs; i++) {
				if ((satsys(obs[i].sat, NULL)&myprcopt.navsys) &&
					myprcopt.exsats[obs[i].sat - 1] != 1) {
					obs[n++] = obs[i];
				}
			}
			if (n <= 0) continue;

			printf("obs num : %d\n", n);

			//rtkpos(&rtk,obs,n,&navs);
			myPPP(&rtk, obs, n, &navs);

			rtk.rb[0] = -2279828.943;  //
			rtk.rb[1] = 5004706.506;
			rtk.rb[2] = 3219777.437;
			//ecef2enu(rtk.sol.rr,pos,e);
			myxyz2neu(rtk.rb, rtk.sol.rr, e, FE_WGS84, RE_WGS84);
			printf("%s : pos-->X:%.3f  %.3f  %.3f \n", timestr, rtk.sol.rr[0], rtk.sol.rr[1], rtk.sol.rr[2]);
			printf("%s : NEU--> %.3f  %.3f  %.3f \n", timestr, e[0], e[1], e[2]);

			//fprintf(recordfile,"%s %16.3f %16.3f %16.3f\n",timestr,rtk.sol.rr[0],rtk.sol.rr[1],rtk.sol.rr[2]);
			fprintf(recordfile, "%s %16.3f %16.3f %16.3f\n", timestr, e[0], e[1], e[2]);

			if (mode == 0) { /* forward/backward */
				if (!solstatic) {
					outsol(logfile, &rtk.sol, rtk.rb, &mysolopt);
				} else if (time.time == 0 || pri[rtk.sol.stat] <= pri[sol.stat]) {
					sol = rtk.sol;
					for (i = 0; i < 3; i++) rb[i] = rtk.rb[i];
					if (time.time == 0 || timediff(rtk.sol.time, time) < 0.0) {
						time = rtk.sol.time;
					}
				}
			} else if (!revs) { /* combined-forward */
				if (isolf >= nepoch) return;
				solf[isolf] = rtk.sol;
				for (i = 0; i < 3; i++) rbf[i + isolf * 3] = rtk.rb[i];
				isolf++;
			} else {  /* combined-backward */
				if (isolb >= nepoch) return;
				solb[isolb] = rtk.sol;
				for (i = 0; i < 3; i++) rbb[i + isolb * 3] = rtk.rb[i];
				isolb++;
			}

		}
		//ÿ�ν��㶼���뽫��2��ֵ��0
		iobsr = 0;
		iobsu = 0;

		/* get stream server status */
		/* show stream server status */
		for (i = 0, p = buff; i < 1; i++) {
			p += sprintf(p, "%c", ss[strstat[i] + 1]);
		}

		printf("%s [%s] %10d B %7d bps %s\n",
			time_str(utc2gpst(timeget()), 0), buff, strbyte[0], bps[0], strmsg);

		for (i = 0, p = buff; i < 1; i++) {
			p += sprintf(p, "%c", ss[strstat_ssr[i] + 1]);
		}

		printf("%s [%s] %10d B %7d bps %s\n",
			time_str(utc2gpst(timeget()), 0), buff, strbyte_ssr[0], bps_ssr[0], strmsg_ssr);

		sleepms(dispint - (tickget() - t1));
	}
}

/* �Զ�������ݴ���������ΪPPP�����������
Author: Zhen.Li
*/
extern int mypost(const prcopt_t *popt, const solopt_t *sopt) {
	int testc = 0, num = 0;
	int nu = 0, nr = 0, mode = 0;
	rtk_t rtk;
	gtime_t time = { 0 };
	sol_t sol = { { 0 } };
	obsd_t obs[MAXOBS * 2]; /* for rover and base */
	double rb[3] = { 0 };
	int i, nobs, n, solstatic, pri[] = { 0, 1, 2, 3, 4, 5, 1, 6 };
	FILE* logfile = NULL;
	char blqfilepath[MAXSTRPATH] = "blq.dat";  //����blq�ļ�
	char timestr[50] = { 0 };

	logfile = fopen("log/ppp.log", "w+");

	solstatic = sopt->solstatic && (popt->mode == PMODE_STATIC || popt->mode == PMODE_PPP_STATIC);

	//rtk ������ʼ��
	//rtkinit(&rtk,popt);
	myrtkinit(&rtk, popt);

	readsp3("data/igs18290.sp3", &navs, 0);
	readsp3("data/igs18291.sp3", &navs, 0);
	readsp3("data/igs18292.sp3", &navs, 0);

	// read clk file
	readrnxc("data/igs18290.clk_30s", &navs);
	readrnxc("data/igs18291.clk_30s", &navs);
	readrnxc("data/igs18292.clk_30s", &navs);


	//��ȡ�۲�����
	readrnx("data/bjfs0260.15o", 1, "", &obss, NULL, stas + 0);

	//��ȡ�㲥����
	readrnx("data/brdc0260.15n", 1, "", NULL, &navs, NULL);

	readpcv("data/igs08.atx", &pcvsr);  //��ȡ��վ������pcv�ļ�������atx�ļ���ͬʱ���������Ǻͽ��ջ�����λ�������ݣ�ȫ������pcvsr��

	//�������Ǻͽ��ջ���������λ����
	setpcv(obss.n > 0 ? obss.data[0].time : timeget(), popt, &navs, &pcvsr, &pcvsr, stas); //�������ͳһ��pcvsr

	//��ȡ���ƫ��dcb�ļ�,���������ز���λ���ݵ��ӳٴ���
	readdcb("data/P1P21501.DCB", &navs);

	//readotl(popt,blqfilepath,stas);  //��ȡ����ϫblq�ļ�

	//������ɺ����antpos���н��ջ�ƫ�ĸ���

	//��������������
	nepoch = sortobs(&obss);
	/* delete duplicated ephemeris */
	uniqnav(&navs);


	//����rb-2267750.574	5009156.111	3221291.833
	//-2267745.5223  5009152.2930  3221292.3765
	//-2148744.405  4426641.2067 4044655.8480
	// -2420721.3846  -3439856.1126  4778532.7479
	// -2.32308096061529e+06 5.59721805372752e+05 5.89411096921164e+06
	rtk.rb[0] = -2148744.405;  //
	rtk.rb[1] = 4426641.2067;
	rtk.rb[2] = 4044655.8480;


	//�˴������ο�procpos���������nobsʵ���������Ǹ���
	while ((nobs = inputobs(obs, rtk.sol.stat, popt)) >= 0) {
		num++;

		/*	if( num>1400)
			{
			continue;;
			}*/


		/* exclude satellites */
		for (i = n = 0; i < nobs; i++) {
			if ((satsys(obs[i].sat, NULL)&popt->navsys) &&
				popt->exsats[obs[i].sat - 1] != 1
				) {
				obs[n++] = obs[i];
			}
		}
		if (n <= 0) continue;

		//��д�����滻��rtkpos
		//if (!rtkpos(&rtk,obs,n,&navs)) continue;
		time2str(obs[0].time, timestr, 3);
		//printf("%s\n",timestr);

		myPPP(&rtk, obs, n, &navs);

		printf("->������Ԫ:%d--%s\n", num, timestr);
		printf("satellites:->");
		for (i = 0; i < nobs; i++) {
			printf("%02d ", obs[i].sat);
		}
		printf("\n");

		///* count rover/base station observations */
		//for (nu=0;nu   <n&&obs[nu   ].rcv==1;nu++) ;
		//for (nr=0;nu+nr<n&&obs[nu+nr].rcv==2;nr++) ;
		////�������ppp���ݴ���ĺ���
		//pppos(&rtk,obs,nu,&navs);

		if (mode == 0) { /* forward/backward */
			if (!solstatic) {
				outsol(logfile, &rtk.sol, rtk.rb, sopt);
			} else if (time.time == 0 || pri[rtk.sol.stat] <= pri[sol.stat]) {
				sol = rtk.sol;
				for (i = 0; i < 3; i++) rb[i] = rtk.rb[i];
				if (time.time == 0 || timediff(rtk.sol.time, time) < 0.0) {
					time = rtk.sol.time;
				}
			}
		} else if (!revs) { /* combined-forward */
			if (isolf >= nepoch) return;
			solf[isolf] = rtk.sol;
			for (i = 0; i < 3; i++) rbf[i + isolf * 3] = rtk.rb[i];
			isolf++;
		} else {  /* combined-backward */
			if (isolb >= nepoch) return;
			solb[isolb] = rtk.sol;
			for (i = 0; i < 3; i++) rbb[i + isolb * 3] = rtk.rb[i];
			isolb++;
		}

	}

	fclose(g_mylog);

	if (mode == 0 && solstatic&&time.time != 0.0) {
		sol.time = time;
		outsol(logfile, &sol, rb, sopt);
	}
	rtkfree(&rtk);
	fclose(logfile);

	testc = 0;

	return 0;
}





