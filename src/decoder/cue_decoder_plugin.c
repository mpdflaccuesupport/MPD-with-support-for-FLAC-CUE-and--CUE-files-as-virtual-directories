

#include "config.h" /* must be first for large file support */

#include "uri.h"
#include "../cue/cue_tag.h"

#ifdef HAVE_CUE /* libcue */
#include <libcue/libcue.h>
#endif

#include "tag.h"
#include "decoder_plugin.h"

#include <glib.h>

#include <assert.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>

#include <stdlib.h>
#include <string.h>

char* cue_track(	const char* pathname,		const unsigned int tnum);
unsigned int cue_vtrack_tnum(const char* fname);
static struct tag *cue_cue_tag_load(const char *file,	const unsigned int tnum);
struct tag *cue_tag_dup(const char *file);
char * cue_file_get_filename(const char *filename,unsigned tnum);
long cue_container_track_times(const char* pathname,const unsigned int tnum, int flag);
struct tag *flac_tag_track_dup(const char* pathname,const unsigned int tnum);
struct Cd *cue_get_cd(const char *pathname);
void remove_suffix(char *str,char pat);
char *cue_locate_audio_container_file(const char *pathname,unsigned tnum);
  
struct Cd *cue_get_cd(const char *pathname)
{
	struct Cd *cd=NULL;
	// extract name of cue file
	FILE *cs_file = fopen(pathname, "rt");

	cd = cue_parse_file(cs_file);
	fclose(cs_file);

	if(cd==NULL)
		return NULL;
	else 
		return cd;
}

long cue_container_track_times(const char* pathname,const unsigned int tnum, int flag)
{
	struct Cd *cd=NULL;
	unsigned int n_tracks=0;
	long time_ms=0;
	
	char *cue_filename=g_strdup(pathname);
	remove_suffix(cue_filename,'/');

	
	// check that filename ends in .cue
	// check that cue file actually exists
	// check that we succesfuly parse the cue file
	if ( !g_str_has_suffix(cue_filename,".cue")
	     || 0 != access (cue_filename, R_OK)
	     ||  (( cd = cue_get_cd(cue_filename) ) == NULL)) {
		g_free(cue_filename);
		return 0;
	}
	
	g_free(cue_filename);
	
	n_tracks=(unsigned) cd_get_ntrack(cd);

	if ( tnum <= n_tracks && flag == 0 ) {
		// return start time of track
		struct Track * tr = cd_get_track(cd, tnum);
		time_ms =  (track_get_start(tr) );
		time_ms*= 1000.0 /75.0;
		free(tr);
	} else if ( tnum < n_tracks && flag == 1  ){
		// return end time of track, for all but the last track
		struct Track * tr = cd_get_track(cd, tnum);
		time_ms = track_get_start(tr);
		time_ms+=track_get_length(tr);
		time_ms =time_ms*1000.0 /75.0;
		free(tr);
	}
	
	return time_ms;

}

void remove_suffix(char *str,char pat)
{
	char *ptr =NULL;
	ptr = strrchr(str,pat);
	if( ptr != NULL )
		*ptr = '\0';
}

char *cue_locate_audio_container_file(const char *pathname,unsigned tnum)
{
	char *cue_filename=g_strdup(pathname); 
	char *container_filename=NULL;  

	// make sure we're trying to work against a file ending with ".cue"

	// if file does't exists, assume it's virtual and try moving up one level
	// to find the actual cue file''
	if (	 access (cue_filename, R_OK) != 0 ) {
		remove_suffix(cue_filename,'/');
		if (  !g_str_has_suffix(cue_filename,".cue")){
			return NULL;
		}    
	}

	// check that we ere either passed a filename ending in ".cue"
	// or located one above, based on what was passed.
  
	if  ( ! g_str_has_suffix(cue_filename,".cue"))
		return NULL;

	// get filename referenced inside cue file
	container_filename=cue_file_get_filename(cue_filename,tnum);
	g_free(cue_filename);
	if(container_filename != NULL )  {
		char *directory = g_strdup(pathname);
		remove_suffix(directory, '/');
		container_filename=g_strconcat(directory,"/", container_filename,NULL);
		g_free(directory);
	}

	// file was located in the same directory as cue file
	if ( access (container_filename, R_OK) ==0 ) 
		return container_filename;

	g_free(container_filename);

	// Now, try to guess the audio/container filename by removing the cue suffix
	// "CDImage.flac.cue"
	container_filename = g_strdup(pathname);
	remove_suffix(container_filename, '.');

	if ( access (container_filename, R_OK) !=0 
	     ) {
		g_free(container_filename);
		return NULL;
	}

	return container_filename;

}


char*
cue_track(	const char* pathname,
	const unsigned int tnum)
{
	struct Cd *cd=NULL;
	unsigned int num_tracks=0;
	char* suffix=NULL;

	if ( ( cd = cue_get_cd(pathname) ) == NULL) 
		return NULL;

	num_tracks=(unsigned) cd_get_ntrack(cd);

	if ( tnum > num_tracks ) {
		return NULL;
	}

	//	char *container_filename=cue_file_get_filename(pathname,tnum);
	char *container_filename=cue_locate_audio_container_file(pathname,tnum);
	if( container_filename != NULL)	{
		suffix = uri_get_suffix( container_filename);
		return g_strdup_printf("track_%03u.%s", tnum,suffix);
	  
	} else {
		return NULL;
	}
}

unsigned int
cue_vtrack_tnum(const char* fname)
{
	/* find last occurrence of '_' in fname
	 * which is hopefully something like track_xxx.cue
	 * another/better way would be to use tag struct
	 */
	char* ptr = strrchr(fname, '_');
	if (ptr == NULL)
		return 0;

	// copy ascii tracknumber to int
	return (unsigned int)strtol(++ptr, NULL, 10);
}

static struct tag *
cue_cue_tag_load(const char *file,	const unsigned int tnum)
{	
	struct tag* tag = NULL;
	char* char_tnum = NULL;
	char* ptr = NULL;
	//	unsigned int sample_rate = 0; 
#ifdef HAVE_CUE /* libcue */
	FILE* cs_file; 
#endif /* libcue */

	ptr = g_strdup(file);
	remove_suffix(ptr,'/');

#ifdef HAVE_CUE /* libcue */

	cs_file = fopen(ptr, "rt");
	g_free(ptr);
	if (cs_file != NULL) {
		tag = cue_tag_file(cs_file, tnum);
		fclose(cs_file);
	}
#else
	g_free(ptr);
#endif /* libcue */

	if (tag == NULL)
		return NULL;

	char_tnum = g_strdup_printf("%u", tnum);

	if (char_tnum != NULL) {
		tag_add_item(tag, TAG_TRACK, char_tnum);
		g_free(char_tnum);
	}

	//	if (sample_rate != 0)
	//	{
	//		tag->time = (unsigned int)(track_time/sample_rate);
	//	}

	return tag;
}


char * cue_file_get_filename(const char *filename,unsigned tnum)
{
	struct Cd *cd;


	FILE *cue_file = fopen(filename, "rt");
	if (cue_file == NULL)
		return NULL;

	cd = cue_parse_file(cue_file);
	fclose (cue_file);

	if ( cd == NULL)
		return NULL;

	// no container directory if there is only one track
	int ntrack =cd_get_ntrack(cd);
	if (ntrack == 1)
		return NULL;

	return track_get_filename( cd_get_track( cd,tnum));
}

static const char *const cue_suffixes[] = { "cue", NULL };
static const char *const cue_mime_types[] = {
	"application/cue",
	"application/x-cue",
	"audio/cue",
	"audio/x-cue",
	NULL
};

const struct decoder_plugin cue_decoder_plugin = {
	.name = "cue",
	.stream_decode = NULL, //cue_decode,
	.tag_dup = NULL, //cue_tag_dup,
	.suffixes = cue_suffixes,
	.mime_types = cue_mime_types,
	.container_scan = cue_track,
	.container_track_tag_dup =cue_cue_tag_load,
	.container_track_times =  cue_container_track_times
};

//#endif /* HAVE_CUE */

