// freqtop.c
//
// by Abraham Stolk.
// This software falls under MIT license.

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>
#include <string.h>
#include <inttypes.h>
#include <signal.h>
#include <sys/ioctl.h>

#if defined(__FreeBSD__)
#	include <sys/types.h>
#	include <sys/sysctl.h>
#endif


static int termw=0, termh=0;
static int doubleres=0;
static int blend=1;
static unsigned char termbg[3] = { 0,0,0 };

static int imw=0;
static int imh=0;
static uint32_t* im=0;
static char* legend=0;

static int tabw=0;
static int barw=0;
static int barh=0;
static int res=0;
static int marginx=0;

static int resized=1;

static int highest_freq;	// highest max freq over all cpus.


#if defined(_WIN64)
#	include <windows.h>
static void get_terminal_size(void)
{
	const HANDLE hStdout = GetStdHandle( STD_OUTPUT_HANDLE );
	CONSOLE_SCREEN_BUFFER_INFO info;
	GetConsoleScreenBufferInfo( hStdout, &info );
	termw = info.dwSize.X;
	termh = info.dwSize.Y;
	if ( !termw ) termw = 80;
}
static int oldcodepage=0;
static void set_console_mode(void)
{
	DWORD mode=0;
	const HANDLE hStdout = GetStdHandle( STD_OUTPUT_HANDLE );
	GetConsoleMode( hStdout, &mode );
	mode = mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
	SetConsoleMode( hStdout, mode );
	oldcodepage = GetConsoleCP();
	SetConsoleCP( 437 );
	doubleres = 1;
}
#else
static void get_terminal_size(void)
{
	FILE* f = popen( "stty size", "r" );
	if ( !f )
	{
		fprintf( stderr, "Failed to determine terminal size using stty.\n" );
		exit( 1 );
	}
	const int num = fscanf( f, "%d %d", &termh, &termw );
	assert( num == 2 );
	pclose( f );
}
static void set_console_mode()
{
	doubleres=1;
}
#endif



static void setup_image( int num_cpus )
{
	if (im) free(im);
	if (legend) free(legend);

	imw = termw;
	imh = 2*(termh-1);
	const size_t sz = imw*imh*4;
	im = (uint32_t*) malloc(sz);
	memset( im, 0x00, sz );

	legend = (char*) malloc( imw*(imh/2) );
	memset( legend, 0x00, imw*(imh/2) );

	// Figure out layout.
	tabw = (imw-4) / num_cpus;
	tabw = tabw < 2 ? 2 : tabw;
	const int bspa = tabw-1;
	barw = bspa > 4 ? 4 : bspa;
	barh = imh-4;
	marginx = (imw - tabw*num_cpus)/2;
	marginx += (bspa-barw)/2;

	// Draw border into image.
	for ( int y=0; y<imh; ++y )
		for ( int x=0; x<imw; ++x )
		{
			uint32_t b = 0x80 + (y/2) * 0xff / imh;
			uint32_t g = 0xff - b;
			uint32_t r = 0x00;
			uint32_t a = 0xff;
			uint32_t colour = a<<24 | b<<16 | g<<8 | r<<0;
			im[y*imw+x] = x==0 || x==imw-1 || y==0 || y==imh-1 ? colour : 0x0;
		}
}


static void setup_legend( int num_cpus, int* freq_bas, int* freq_min, int* freq_max )
{
	// Find highest freq
	highest_freq=-1;
	for (int i=0; i<num_cpus; ++i)
		highest_freq = freq_max[i] > highest_freq ? freq_max[i] : highest_freq;

	// Set up the legend.
	char label_bas[16];
	char label_min[16];
	char label_max[16];
	snprintf( label_bas, sizeof(label_bas), "%3.1f", freq_bas[0]  / 1000000.0f );
	snprintf( label_min, sizeof(label_min), "%3.1f", freq_min[0]  / 1000000.0f );
	snprintf( label_max, sizeof(label_max), "%3.1f", highest_freq / 1000000.0f );
	int y=1; int x=1;
	sprintf( legend + y * imw + x, "%s", label_max );
	if ( freq_bas[0] != freq_max[0] )
	{
		y=(imh/2) - (barh/2) * freq_bas[0] / (float) highest_freq;
		sprintf( legend + y * imw + x, "%s", label_bas );
	}
	y=(imh/2) - (barh/2) * freq_min[0] / (float) highest_freq;
	sprintf( legend + y * imw + x, "%s", label_min );
}


static void sigwinchHandler(int sig)
{
	resized = 1;
}


static struct termios orig_termios;

void disableRawMode()
{
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}


void enableRawMode()
{
	tcgetattr(STDIN_FILENO, &orig_termios);
	atexit(disableRawMode);
	struct termios raw = orig_termios;
	raw.c_lflag &= ~(ECHO);				// Don't echo key presses.
	raw.c_lflag &= ~(ICANON);			// Read by char, not by line.
	raw.c_cc[VMIN] = 0;				// No minimum nr of chars.
	raw.c_cc[VTIME] = 0;				// No waiting time.
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}


#define RESETALL  	"\x1b[0m"

#define CURSORHOME	"\x1b[1;1H"

#define CLEARSCREEN	"\x1b[2J"


#if defined(_WIN64)
#	define HALFBLOCK "\xdf"		// Uses IBM PC Codepage 437 char 223
#else
#	define HALFBLOCK "▀"		// Uses Unicode char U+2580
#endif

// Note: image has alpha pre-multied. Mimic GL_ONE + GL_ONE_MINUS_SRC_ALPHA
#define BLEND \
{ \
	const int t0 = 255; \
	const int t1 = 255-a; \
	r = ( r * t0 + termbg[0] * t1 ) / 255; \
	g = ( g * t0 + termbg[1] * t1 ) / 255; \
	b = ( b * t0 + termbg[2] * t1 ) / 255; \
}

static void print_image_double_res( int w, int h, unsigned char* data, char* legend )
{
	if ( h & 1 )
		h--;
	const int linesz = 32768;
	char line[ linesz ];

	for ( int y=0; y<h; y+=2 )
	{
		const unsigned char* row0 = data + (y+0) * w * 4;
		const unsigned char* row1 = data + (y+1) * w * 4;
		line[0] = 0;
		for ( int x=0; x<w; ++x )
		{
			char legendchar = legend ? *legend++ : 0;
			// foreground colour.
			strncat( line, "\x1b[38;2;", sizeof(line) - strlen(line) - 1 );
			char tripl[80];
			unsigned char r = *row0++;
			unsigned char g = *row0++;
			unsigned char b = *row0++;
			unsigned char a = *row0++;
			if ( legendchar ) r=g=b=a=0xff;
			if ( blend )
				BLEND
			snprintf( tripl, sizeof(tripl), "%d;%d;%dm", r,g,b );
			strncat( line, tripl, sizeof(line) - strlen(line) - 1 );
			// background colour.
			strncat( line, "\x1b[48;2;", sizeof(line) - strlen(line) - 1 );
			r = *row1++;
			g = *row1++;
			b = *row1++;
			a = *row1++;
			if ( legendchar ) r=g=b=a=0x00;
			if ( blend )
				BLEND
			if ( legendchar )
				snprintf( tripl, sizeof(tripl), "%d;%d;%dm%c", r,g,b,legendchar );
			else
				snprintf( tripl, sizeof(tripl), "%d;%d;%dm" HALFBLOCK, r,g,b );
			strncat( line, tripl, sizeof(line) - strlen(line) - 1 );
		}
		strncat( line, RESETALL, sizeof(line) - strlen(line) - 1 );
		if ( y==h-1 )
			printf( "%s", line );
		else
			puts( line );
	}
}


int get_cpu_stat( int cpu, const char* name )
{
	char fname[128];
	char line [128];
	snprintf( fname, sizeof(fname), "/sys/devices/system/cpu/cpufreq/policy%d/%s", cpu, name );
	FILE* f = fopen( fname, "rb" );
	if ( !f )
		return -1;
	const int numread = fread( line, 1, sizeof(line), f );
	assert( numread > 0 );
	fclose(f);
	return atoi( line );
}


int get_cpu_coreid( int cpu )
{
	char fname[128];
	char line [128];
	snprintf( fname, sizeof(fname), "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu );
	FILE* f = fopen( fname, "rb" );
	if ( !f ) return -1;
	const int numread = fread( line, 1, sizeof(line), f );
	assert( numread > 0 );
	fclose(f);
	return atoi( line );
}


#if defined(__FreeBSD__)
static int* mib_fr=0;
static int get_cur_freq_via_sysctl( int num_cpus, int cpunr )
{
	if ( !mib_fr )
	{
		mib_fr = (int*) malloc(num_cpus * 4 * sizeof(int));
		for ( int cpu=0; cpu<num_cpus; ++cpu )
		{
			char nm[80];
			snprintf( nm, sizeof(nm), "dev.cpu.%d.freq", cpu );
			size_t num=4;
			const int r0 = sysctlnametomib( nm, mib_fr+cpu*4, &num );
			assert(!r0);
		}
	}
	int rv=0;
	size_t sz = sizeof(rv);
	const int r1 = sysctl( mib_fr+cpunr*4, 4, &rv, &sz, 0, 0 );
	assert(!r1);
	assert(sz==sizeof(rv));
	return 1000 * rv;
}
#endif


#if !defined(__FreeBSD__)
static uint32_t *prev=0;
static uint32_t *curr=0;
// Reads for each cpu: how many jiffies were spent in each state:
//   user, nice, system, idle, iowait, irq, softirq
// NOTE: nice is a sub category of user. iowait is a sub category of idle. (soft)irq are sub categories too.
void get_usages( int num_cpus, float* usages )
{
	if ( !prev || !curr )
	{
		const size_t sz = sizeof(uint32_t) * 7 * num_cpus;
		prev = (uint32_t*) malloc( sz );
		curr = (uint32_t*) malloc( sz );
		memset( prev, 0, sz );
		memset( curr, 0, sz );
	}

	static FILE* f = 0;
	if ( !f )
	{
		f = fopen( "/proc/stat", "rb" );
		assert( f );
	}
	char info[16384];
	const int numr = fread( info, 1, sizeof(info), f );
	assert( numr > 0 );
	rewind(f);
	if ( numr < sizeof(info) )
	{
		info[numr] = 0;
		for ( int cpu=0; cpu<num_cpus; ++cpu )
		{
			char tag[16];
			snprintf( tag, sizeof(tag), "cpu%d", cpu );
			const char* s = strstr( info, tag );
			assert( s );
			int cpunr;
			uint32_t* prv = prev + cpu * 7;
			uint32_t* cur = curr + cpu * 7;
			const int numscanned = sscanf( s, "cpu%d %u %u %u %u %u %u %u", &cpunr, cur+0, cur+1, cur+2, cur+3, cur+4, cur+5, cur+6 );
			assert( numscanned == 8 );
			assert( cpunr == cpu );
			uint32_t deltas[7];
			for ( int i=0; i<7; ++i )
			{
				deltas[i] = cur[i] - prv[i];
				prv[i] = cur[i];
			}
			const uint32_t user = deltas[0];
			const uint32_t syst = deltas[2];
			const uint32_t idle = deltas[3];
			const uint32_t work = user + syst;
			usages[ cpu ] = work / (float) (idle+work);
		}
	}
}
#endif


#if defined(__FreeBSD__)
static uint64_t *prev=0;
static uint64_t *curr=0;
static int* mib_cp=0;
static void get_usages_via_sysctl( int num_cpus, float* usages )
{
	if ( !mib_cp )
	{
		mib_cp = (int*) malloc(2 * sizeof(int));
		size_t num=2;
		const int r1 = sysctlnametomib( "kern.cp_times", mib_cp, &num );
		assert(!r1);
		assert(num==2);
		size_t sz=0;
		const int r2 = sysctl( mib_cp, 2, 0, &sz, 0, 0 );
		assert(!r2);
		fprintf(stderr,"sz for mib_cp is %lu\n", sz);
	}
	if ( !prev || !curr )
	{
		const size_t sz = sizeof(uint64_t) * 5 * num_cpus;
		prev = (uint64_t*) malloc( sz );
		curr = (uint64_t*) malloc( sz );
		memset( prev, 0, sz );
		memset( curr, 0, sz );
	}

	size_t sz = num_cpus * 5 * sizeof(uint64_t);
	const int r3 = sysctl( mib_cp, 2, curr, &sz, 0, 0 );
	assert(!r3);

	for ( int cpu=0; cpu<num_cpus; ++cpu )
	{
		uint64_t deltas[5];
		for ( int i=0; i<5; ++i )
		{
			deltas[i] = curr[cpu*5+i] - prev[cpu*5+i];
			prev[cpu*5+i] = curr[cpu*5+i];
		}
		const uint64_t user = deltas[0];
		const uint64_t syst = deltas[2];
		const uint64_t idle = deltas[4];
		usages[cpu] = (user+syst) / (float)(user+syst+idle);
	}
}
#endif



int main( int argc, char* argv[] )
{
	// Parse environment variable for terminal background colour.
	const char* imcatbg = getenv( "IMCATBG" );
	if ( imcatbg )
	{
		const int bg = strtol( imcatbg+1, 0, 16 );
		termbg[ 2 ] = ( bg >>  0 ) & 0xff;
		termbg[ 1 ] = ( bg >>  8 ) & 0xff;
		termbg[ 0 ] = ( bg >> 16 ) & 0xff;
		blend = 1;
	}

	// Step 0: Windows cmd.exe needs to be put in proper console mode.
	set_console_mode();

	// How many cores in this system?
	const int num_cpus = sysconf( _SC_NPROCESSORS_ONLN );
	fprintf( stderr, "Found %d cpus.\n", num_cpus );

	int freq_bas[ num_cpus ];	// Base frequencies.
	int freq_min[ num_cpus ];	// Min frequencies.
	int freq_max[ num_cpus ];	// Max frequencies.
	int freq_cur[ num_cpus ];	// Current frequencies.
	float usages[ num_cpus ];	// Core loads.
	int coreids [ num_cpus ];	// Core ids (could map to hyperthread sibling.)
	int rank    [ num_cpus ];	// Ordering when displaying bars.
	int policy  [ num_cpus ];	// Which scaling policy to use. Sometimes (rPi4) cores share 1 policy.
	int lastpolicy=-1;		// Which policy did we see last.

#if defined(__FreeBSD__)
	(void) policy;
	int mib_fl[num_cpus][4];
	for ( int cpu=0; cpu<num_cpus; ++cpu )
	{
		char nm[80];
		snprintf( nm, sizeof(nm), "dev.cpu.%d.freq_levels", cpu );
		size_t num=4;
		const int r0 = sysctlnametomib( nm, mib_fl[cpu], &num );
		assert(!r0);
		size_t sz=0;
		const int r1 = sysctl( mib_fl[cpu], 4, 0, &sz, 0, 0 );
		assert(!r1);
		assert(sz);
		char fl[1024];
		const int r2 = sysctl( mib_fl[cpu], 4, fl, &sz, 0, 0 );
		assert(!r2);
		char* last = fl+sz-1;
		char* p = fl;
		int fnr=0;
		int watt_max=0;
		int watt_min=0;
		while( p < last && p>=fl && fnr<20 )
		{
			char* mid=0;
			char* end=0;
			int fr = (int) strtol(p,     &mid, 10);
			int wa = (int) strtol(mid+1, &end, 10);
			p = end;
			fr *= 1000;
			if (!fnr || fr<freq_min[cpu]) freq_min[cpu]=fr;
			if (!fnr || fr>freq_max[cpu]) freq_max[cpu]=fr;
			if (!fnr || wa<watt_min) watt_min=wa;
			if (!fnr || wa>watt_max) watt_max=wa;
			fnr++;
		}
		fprintf(stderr,"cpu %d scales between %d .. %d with wattage between %d .. %d\n", cpu, freq_min[cpu], freq_max[cpu], watt_min, watt_max);
		coreids[cpu] = cpu; // BAD: We assume no hyperthreading on BSD.
		freq_bas[cpu] = (freq_max[cpu] + freq_min[cpu])/2;
	}
#else
	for ( int cpu=0; cpu<num_cpus; ++cpu )
	{
#if 0
		const char* minstat = "scaling_min_freq";
		const char* maxstat = "scaling_max_freq";
		const char* basstat = "base_frequency";
#else
		const char* minstat = "cpuinfo_min_freq";
		const char* maxstat = "cpuinfo_max_freq";
		const char* basstat = "base_frequency";
#endif
		freq_min[ cpu ] = get_cpu_stat( cpu, minstat );
		freq_max[ cpu ] = get_cpu_stat( cpu, maxstat );
		freq_bas[ cpu ] = get_cpu_stat( cpu, basstat );
		if ( freq_bas[ cpu ] <= 0 )
			freq_bas[ cpu ] = freq_max[ cpu ];
		if ( freq_min[ cpu ] < 0 || freq_max[ cpu ] < 0 )
		{
			policy[ cpu ] = lastpolicy;
			freq_min[ cpu ] = freq_min[ lastpolicy ];
			freq_bas[ cpu ] = freq_bas[ lastpolicy ];
			freq_max[ cpu ] = freq_max[ lastpolicy ];
		}
		else
		{
			policy[ cpu ] = cpu;
			lastpolicy = cpu;
		}
		coreids [ cpu ] = get_cpu_coreid( cpu );
		fprintf( stderr, "cpu %d(core%d): %d/%d/%d\n", cpu, coreids[cpu], freq_min[cpu], freq_bas[cpu], freq_max[cpu] );
	}
#endif

	int corehi=-1;
	for ( int i=0; i<num_cpus; ++i )
		if ( coreids[ i ] > corehi )
			corehi = coreids[ i ];
	if ( corehi>-1 && corehi < num_cpus-1 )
	{
		// Hyperthreading: reorder the cpus so that physical and hyperthread core are paired.
		int written=0;
		for ( int physcore=0; physcore<=corehi; ++physcore )
		{
			for ( int i=0; i<num_cpus; ++i )
				if ( physcore == coreids[i] )
					rank[ written++ ] = i;
		}
		assert( written == num_cpus );
	}
	else
	{
		// No hyperthreading: show cores in nominal order.
		for ( int i=0; i<num_cpus; ++i )
			rank[ i ] = i;
	}
	for ( int i=0; i<num_cpus; ++i )
	{
		fprintf(stderr, "rank %d: %d\n", i, rank[i] );
	}

	const uint32_t ora = 0xff0060b0;

	enableRawMode();

	// Listen to changes in terminal size
	struct sigaction sa;
	sigemptyset( &sa.sa_mask );
	sa.sa_flags = 0;
	sa.sa_handler = sigwinchHandler;
	if ( sigaction( SIGWINCH, &sa, 0 ) == -1 )
		perror( "sigaction" );

	int done=0;
	while ( !done )
	{
		if ( resized )
		{
			printf(CLEARSCREEN);
			get_terminal_size();
			setup_image( num_cpus );
			setup_legend( num_cpus, freq_bas, freq_min, freq_max );
			resized = 0;
		}
		res = (int) roundf( highest_freq / (float)barh );

		char c;
		const int numr = read( STDIN_FILENO, &c, 1 );
		if ( numr == 1 && c == 27 )
			done=1;
	
		for ( int cpu=0; cpu<num_cpus; ++cpu )
		{
#if defined(__FreeBSD__)
			freq_cur[ cpu ] = get_cur_freq_via_sysctl( num_cpus, cpu );
#else
			const int pol = policy[ cpu ];
			freq_cur[ cpu ] = get_cpu_stat( pol, "scaling_cur_freq" );
#endif
		}
		for ( int ra=0; ra<num_cpus; ++ra )
		{
			const int cpu = rank[ ra ];
			const int cid = coreids[ cpu ];
			uint32_t fcur = freq_cur[ cpu ];
			if ( cid != -1 && cid != cpu )
				fcur = freq_cur[ cid ];
			for ( int i=0; i<barh; ++i )
			{
				int f = res * (i+1);
				uint8_t r=0x00;
				uint8_t g=0xc0;
				uint8_t b=0x00;
				uint8_t a=0xff;
			
				if ( f >= freq_min[cpu] ) { r=0xa0; g=0xa0; b=0x00; }
				if ( f >= freq_bas[cpu] ) { r=0xc0; g=0x00; b=0x00; }
				if ( f >  freq_max[cpu] ) { r=0x00; g=0x00; b=0x00; }
				uint32_t lo = 0x30 * i / barh;
				uint32_t hi = 0x30 + 0xc0 * i / barh;
				r = r < lo ? lo : r > hi ? hi : r;
				g = g < lo ? lo : g > hi ? hi : g;
				b = b < lo ? lo : b > hi ? hi : b;
				if ( f >  fcur )
				{
					r=(r>>2);
					g=(g>>2);
					b=(b>>2);
					a=(a>>2);
				}
				uint32_t c = (a<<24) | (b<<16) | (g<<8) | (r<<0);
				for ( int bx=1; bx<=barw; ++bx )
				{
					int x = marginx + ra * tabw + bx;
					int y = imh - 2 - i;
					if ( x < imw )
						im[ y*imw + x ] = c;
				}
			}
		}
#if defined(__FreeBSD__)
		get_usages_via_sysctl( num_cpus, usages );
#else
		get_usages( num_cpus, usages );
#endif
		for ( int r=0; r<num_cpus; ++r )
		{
			const int cpu = rank[ r ];
			int x = marginx + r * tabw;
			int cy = (int) ( (1-usages[cpu]) * barh );
			for ( int by=0; by<=barh; ++by )
			{
				int y = 2 + by;
				if ( x < imw )
					im [ y*imw + x ] = by==cy ? ora : 0x00000000;
			}
		}
		printf( CURSORHOME );
		print_image_double_res( imw, imh, (unsigned char*) im, legend );
		const int NS_PER_MS = 1000000;
		struct timespec ts  = { 0, 64 * NS_PER_MS };
		nanosleep( &ts, 0 );
	}

	free(im);

#if defined(_WIN64)
	SetConsoleCP( oldcodepage );
#endif
	printf( CLEARSCREEN );

	return 0;
}

