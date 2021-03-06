#include <framework/mlt.h>

#include <vlc/vlc.h>

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#define VIDEO_COOKIE 0
#define AUDIO_COOKIE 1

static int consumers = 1;

// Debug code

typedef struct consumer_libvlc_s *consumer_libvlc;

struct consumer_libvlc_s
{
	int id;
	mlt_consumer parent;
	libvlc_instance_t *vlc;
	libvlc_media_t *media;
	libvlc_media_player_t *media_player;
	libvlc_event_manager_t *mp_manager;
	int64_t latest_video_pts;
	int64_t latest_audio_pts;
	mlt_deque frame_queue;
	pthread_mutex_t queue_mutex;
	void *video_imem_data;
	void *audio_imem_data;
	int running;
	int output_to_window;
};

static void log_cb( void *data, int vlc_level, const libvlc_log_t *ctx, const char *fmt, va_list args )
{
	consumer_libvlc self = data;
	int consumer_id = self->id;

	int mlt_level;
	switch ( vlc_level )
	{
		case LIBVLC_DEBUG:
			mlt_level = MLT_LOG_DEBUG;
			break;
		case LIBVLC_NOTICE:
			mlt_level = MLT_LOG_INFO;
			break;
		case LIBVLC_WARNING:
			mlt_level = MLT_LOG_WARNING;
			break;
		case LIBVLC_ERROR:
		default:
			mlt_level = MLT_LOG_FATAL;
	}

	mlt_vlog( MLT_CONSUMER_SERVICE( self->parent ), mlt_level, fmt, args );
}

static void setup_vlc( consumer_libvlc self );
static void setup_vlc_sout( consumer_libvlc self );
static int setup_vlc_window( consumer_libvlc self );
static void setup_vlc_properties( consumer_libvlc self );
static int imem_get( void *data, const char* cookie, int64_t *dts, int64_t *pts,
					 unsigned *flags, size_t *bufferSize, void **buffer );
static void imem_release( void *data, const char* cookie, size_t buffSize, void *buffer );
static int consumer_start( mlt_consumer parent );
static int consumer_stop( mlt_consumer parent );
static int consumer_is_stopped( mlt_consumer parent );
static void consumer_close( mlt_consumer parent );
static void consumer_purge( mlt_consumer parent );
static void mp_callback( const struct libvlc_event_t *evt, void *data );

mlt_consumer consumer_libvlc_init( mlt_profile profile, mlt_service_type type, const char *id, void *arg )
{
	int err;
	mlt_consumer parent = NULL;
	consumer_libvlc self = NULL;

	// Allocate the consumer data structures
	parent = calloc( 1, sizeof( struct mlt_consumer_s ) );
	self = calloc( 1, sizeof( struct consumer_libvlc_s ) );
	assert( parent != NULL && self != NULL );
	err = mlt_consumer_init( parent, self, profile );
	assert( err == 0 );

	mlt_properties properties = MLT_CONSUMER_PROPERTIES( parent );
	mlt_properties_set_lcnumeric( properties, "C" );
	self->parent = parent;

	// Set default libVLC specific properties
	mlt_properties_set_int( properties, "input_image_format", mlt_image_yuv422 );
	mlt_properties_set_int( properties, "input_audio_format", mlt_audio_s16 );
	mlt_properties_set( properties, "output_vcodec", "mp2v" );
	mlt_properties_set( properties, "output_acodec", "mpga" );
	mlt_properties_set_int( properties, "output_vb", 8000000 );
	mlt_properties_set_int( properties, "output_ab", 128000 );
	if ( self->output_to_window )
		mlt_properties_set_data( properties, "output_dst", arg, 0, NULL, NULL );
	else
		mlt_properties_set( properties, "output_dst", ( char* )arg );
	mlt_properties_set( properties, "output_mux", "ps" );
	mlt_properties_set( properties, "output_access", "file" );

	self->vlc = libvlc_new( 0, NULL );
	assert( self->vlc != NULL );

	// Debug code
	libvlc_log_set( self->vlc, log_cb, self );

	self->frame_queue = mlt_deque_init( );
	assert( self->frame_queue != NULL );

	pthread_mutex_init( &self->queue_mutex, NULL );

	parent->start = consumer_start;
	parent->stop = consumer_stop;
	parent->is_stopped = consumer_is_stopped;
	parent->close = consumer_close;
	parent->purge = consumer_purge;

	// Set output_to_window flag if needed
	if ( !strcmp( id, "libvlc_window" ) )
		self->output_to_window = 1;

	return parent;
}

static void setup_vlc_properties( consumer_libvlc self )
{
	mlt_properties properties = MLT_CONSUMER_PROPERTIES( self->parent );

	// Create a consistent snapshot of properties to be used during media_player runtime
	mlt_properties_set_int( properties, "_vlc_width", mlt_properties_get_int( properties, "width" ) );
	mlt_properties_set_int( properties, "_vlc_height", mlt_properties_get_int( properties, "height" ) );
	mlt_properties_set( properties, "_vlc_display_ratio", mlt_properties_get( properties, "display_ratio" ) );
	mlt_properties_set( properties, "_vlc_fps", mlt_properties_get( properties, "fps" ) );
	mlt_properties_set_int( properties, "_vlc_input_image_format", mlt_properties_get_int( properties, "input_image_format" ) );
	mlt_properties_set_int( properties, "_vlc_input_audio_format", mlt_properties_get_int( properties, "input_audio_format" ) );
	mlt_properties_set_int( properties, "_vlc_frequency", mlt_properties_get_int( properties, "frequency" ) );
	mlt_properties_set_int( properties, "_vlc_channels", mlt_properties_get_int( properties, "channels" ) );
	mlt_properties_set( properties, "_vlc_window_type", mlt_properties_get( properties, "window_type" ) );
	mlt_properties_set( properties, "_vlc_output_dst", mlt_properties_get( properties, "output_dst" ) );
	mlt_properties_set_data( properties, "_vlc_output_dst", mlt_properties_get_data( properties, "output_dst", NULL ), 0, NULL, NULL );
	mlt_properties_set_int( properties, "_vlc_output_vb", mlt_properties_get_int( properties, "output_vb" ) );
	mlt_properties_set_int( properties, "_vlc_output_ab", mlt_properties_get_int( properties, "output_ab" ) );
	mlt_properties_set( properties, "_vlc_output_vcodec", mlt_properties_get( properties, "output_vcodec" ) );
	mlt_properties_set( properties, "_vlc_output_acodec", mlt_properties_get( properties, "output_acodec" ) );
	mlt_properties_set( properties, "_vlc_output_access", mlt_properties_get( properties, "output_access" ) );
	mlt_properties_set( properties, "_vlc_output_mux", mlt_properties_get( properties, "output_mux" ) );

}

static void setup_vlc( consumer_libvlc self )
{
	mlt_properties properties = MLT_CONSUMER_PROPERTIES( self->parent );

	// imem setup phase

	// Copy some properties to make them consistent throughout media_player runtime
	setup_vlc_properties( self );

	// Allocate buffers
	char imem_video_conf[ 512 ];
	char imem_audio_conf[ 512 ];
	char imem_get_conf[ 512 ];
	char imem_release_conf[ 512 ];
	char imem_data_conf[ 512 ];

	// Translate input_image_format and input_audio_format from MLT format to VLC format
	const char *vlc_input_vcodec;

	const char RV24[] = "RV24";
	const char RGBA[] = "RGBA";
	const char YUV2[] = "YUY2";
	const char I420[] = "I420";

	switch ( mlt_properties_get_int( properties, "_vlc_input_image_format" ) )
	{
		case mlt_image_rgb24:
			vlc_input_vcodec = RV24;
			break;
		case mlt_image_rgb24a:
			vlc_input_vcodec = RGBA;
			break;
		case mlt_image_yuv422:
			vlc_input_vcodec = YUV2;
			break;
		case mlt_image_yuv420p:
			vlc_input_vcodec = I420;
			break;
		default:
			mlt_log_verbose( MLT_CONSUMER_SERVICE( self->parent ), "Unsupported input_image_format. Defaulting to yuv422.\n" );
			vlc_input_vcodec = YUV2;
			break;
	}

	const char *vlc_input_acodec;

	const char s16l[] = "s16l";
	const char s32l[] = "s32l";
	const char fl32[] = "fl32";

	switch ( mlt_properties_get_int( properties, "_vlc_input_audio_format" ) )
	{
		case mlt_audio_s16:
			vlc_input_acodec = s16l;
			break;
		case mlt_audio_s32le:
			vlc_input_acodec = s32l;
			break;
		case mlt_audio_f32le:
			vlc_input_acodec = fl32;
			break;
		default:
			mlt_log_verbose( MLT_CONSUMER_SERVICE( self->parent ), "Unsupported input_audio_format. Defaulting to s16.\n" );
			vlc_input_acodec = s16l;
			break;
	}


	// We will create media using imem MRL
	sprintf( imem_video_conf, "imem://width=%i:height=%i:dar=%s:fps=%s/1:cookie=0:codec=%s:cat=2:caching=0",
		mlt_properties_get_int( properties, "_vlc_width" ),
		mlt_properties_get_int( properties, "_vlc_height" ),
		mlt_properties_get( properties, "_vlc_display_ratio" ),
		mlt_properties_get( properties, "_vlc_fps" ),
		vlc_input_vcodec );

	// Audio stream will be added as input slave
	sprintf( imem_audio_conf, ":input-slave=imem://cookie=1:cat=1:codec=%s:samplerate=%d:channels=%d:caching=0",
		vlc_input_acodec,
		mlt_properties_get_int( properties, "_vlc_frequency" ),
		mlt_properties_get_int( properties, "_vlc_channels" ) );

	// This configures imem callbacks
	sprintf( imem_get_conf,
		":imem-get=%" PRIdPTR,
		(intptr_t)(void*)&imem_get );

	sprintf( imem_release_conf,
		":imem-release=%" PRIdPTR,
		(intptr_t)(void*)&imem_release );

	sprintf( imem_data_conf,
		":imem-data=%" PRIdPTR,
		(intptr_t)(void*)self );

	// Create media...
	self->media = libvlc_media_new_location( self->vlc, imem_video_conf );
	assert( self->media != NULL );

	// ...and apply configuration parameters.
	libvlc_media_add_option( self->media, imem_audio_conf );
	libvlc_media_add_option( self->media, imem_get_conf );
	libvlc_media_add_option( self->media, imem_release_conf );
	libvlc_media_add_option( self->media, imem_data_conf );

	// Setup sout chain if we're not outputting to window
	if ( !self->output_to_window )
	{
		setup_vlc_sout( self );
	}

	self->id = consumers;
	consumers++;
}

static void setup_vlc_sout( consumer_libvlc self )
{
	assert( self->media != NULL );

	mlt_properties properties = MLT_CONSUMER_PROPERTIES( self->parent );

	char sout_conf[ 512 ];

	// This configures file output
	sprintf( sout_conf, ":sout=#transcode{"
		"vcodec=%s,fps=%s,width=%d,height=%d,vb=%d,"
		"acodec=%s,channels=%d,samplerate=%d,ab=%d}"
		":standard{access=%s,mux=%s,dst=\"%s\"}",
		mlt_properties_get( properties, "_vlc_output_vcodec" ),
		mlt_properties_get( properties, "_vlc_fps" ),
		mlt_properties_get_int( properties, "_vlc_width" ),
		mlt_properties_get_int( properties, "_vlc_height" ),
		mlt_properties_get_int( properties, "_vlc_output_vb" ),
		mlt_properties_get( properties, "_vlc_output_acodec" ),
		mlt_properties_get_int( properties, "_vlc_channels" ),
		mlt_properties_get_int( properties, "_vlc_frequency" ),
		mlt_properties_get_int( properties, "_vlc_output_ab" ),
		mlt_properties_get( properties, "_vlc_output_access" ),
		mlt_properties_get( properties, "_vlc_output_mux" ),
		mlt_properties_get( properties, "_vlc_output_dst" ) );

	libvlc_media_add_option( self->media, sout_conf );
}

static int setup_vlc_window( consumer_libvlc self )
{
	mlt_properties properties = MLT_CONSUMER_PROPERTIES( self->parent );

	char *window_type = mlt_properties_get( properties, "_vlc_window_type" );
	void *window_handle = mlt_properties_get_data( properties, "_vlc_output_dst", NULL );

	if ( window_type != NULL && window_handle != NULL )
	{
		if ( !strcmp( window_type, "nsobject") )
		{
			libvlc_media_player_set_nsobject( self->media_player, window_handle );
		}
		else if ( !strcmp( window_type, "xwindow" ) )
		{
			libvlc_media_player_set_xwindow( self->media_player, ( uint32_t )window_handle );
		}
		else if ( !strcmp( window_type, "hwnd" ) )
		{
			libvlc_media_player_set_hwnd( self->media_player, window_handle );
		}
		else
		{
			// Some unknown window_type was passed
			return 1;
		}

		// Setup finished successfully
		return 0;
	}
	else
	{
		// We failed to get window_type and/or window_handle
		return 1;
	}
}

static int imem_get( void *data, const char* cookie, int64_t *dts, int64_t *pts,
					 uint32_t *flags, size_t *bufferSize, void **buffer )
{
	consumer_libvlc self = data;
	mlt_properties properties = MLT_CONSUMER_PROPERTIES( self->parent );
	mlt_frame frame = NULL;
	// Whether or not fetched frame need releasing
	int cleanup_frame = 0;
	*buffer = NULL;

	int cookie_int = cookie[ 0 ] - '0';

	// Get data if needed
	pthread_mutex_lock( &self->queue_mutex );

	frame = mlt_deque_pop_front( self->frame_queue );

	// If we got frame from queue, we need to release it later
	if ( frame != NULL )
		cleanup_frame = 1;
	else
		frame = mlt_consumer_get_frame( self->parent );

	if ( cookie_int == AUDIO_COOKIE && self->running )
	{
		assert( frame != NULL );
		double speed = mlt_properties_get_double( MLT_FRAME_PROPERTIES( frame ), "_speed" );

		// We terminate imem on pause if needed
		if ( speed == 0.0 && mlt_properties_get_int( properties, "terminate_on_pause" ) )
		{
			self->running = 0;
			pthread_mutex_unlock( &self->queue_mutex );
			return 1;
		}
		else
		{
			// This is used to pass frames to imem_release() if they need cleanup
			self->audio_imem_data = NULL;

			mlt_audio_format afmt = mlt_properties_get_int( properties, "_vlc_input_audio_format" );
			double fps = mlt_properties_get_double( properties, "_vlc_fps" );
			int frequency = mlt_properties_get_int( properties, "_vlc_frequency" );
			int channels = mlt_properties_get_int( properties, "_vlc_channels" );
			int samples = mlt_sample_calculator( fps, frequency, mlt_frame_original_position( frame ) );
			double pts_diff = ( double )samples / ( double )frequency * 1000000.0;

			mlt_frame_get_audio( frame, buffer, &afmt, &frequency, &channels, &samples );
			*bufferSize = mlt_audio_format_size( afmt, samples, channels );

			*pts = self->latest_audio_pts + pts_diff + 0.5;
			*dts = *pts;

			self->latest_audio_pts = *pts;

			if ( cleanup_frame )
				self->audio_imem_data = frame;
			else
				mlt_deque_push_back( self->frame_queue, frame );
		}
	}
	else if ( cookie_int == VIDEO_COOKIE && self->running )
	{
		assert( frame != NULL );
		double speed = mlt_properties_get_double( MLT_FRAME_PROPERTIES( frame ), "_speed" );

		if ( speed == 0.0 && mlt_properties_get_int( properties, "terminate_on_pause" ) )
		{
			self->running = 0;
			pthread_mutex_unlock( &self->queue_mutex );
			return 1;
		}
		else
		{
			self->video_imem_data = NULL;

			double fps = mlt_properties_get_double( properties, "_vlc_fps" );
			double pts_diff = 1.0 / fps * 1000000.0;

			mlt_image_format vfmt = mlt_properties_get_int( properties, "_vlc_input_image_format" );
			int width = mlt_properties_get_int( properties, "_vlc_width" );
			int height = mlt_properties_get_int( properties, "_vlc_height" );
			mlt_frame_get_image( frame, ( uint8_t ** )buffer, &vfmt, &width, &height, 0 );
			*bufferSize = mlt_image_format_size( vfmt, width, height, NULL );

			*pts = self->latest_video_pts + pts_diff;
			*dts = *pts;

			self->latest_video_pts = *pts;

			if ( cleanup_frame )
				self->video_imem_data = frame;
			else
				mlt_deque_push_back( self->frame_queue, frame );
			}
	}
	else if ( self->running )
	{
		// Invalid cookie
		assert( 0 );
	}
	pthread_mutex_unlock( &self->queue_mutex );

	assert( frame != NULL );
	if ( *buffer == NULL )
		return 1;

	return 0;
}

static void imem_release( void *data, const char* cookie, size_t buffSize, void *buffer )
{
	consumer_libvlc self = data;

	int cookie_int = cookie[ 0 ] - '0';

	if ( cookie_int == VIDEO_COOKIE && self->running )
	{
		if ( self->video_imem_data )
		{
			mlt_properties properties = MLT_CONSUMER_PROPERTIES( self->parent );
			mlt_frame frame = self->video_imem_data;
			mlt_events_fire( properties, "consumer-frame-show", frame, NULL );
			mlt_frame_close( frame );
			self->video_imem_data = NULL;
		}
	}
	else if ( cookie_int == AUDIO_COOKIE && self->running )
	{
		if ( self->audio_imem_data )
		{
			mlt_properties properties = MLT_CONSUMER_PROPERTIES( self->parent );
			mlt_frame frame = self->audio_imem_data;
			mlt_events_fire( properties, "consumer-frame-show", frame, NULL );
			mlt_frame_close( frame );
			self->audio_imem_data = NULL;

		}
	}
	else if ( self->running )
	{
		// Invalid cookie
		assert( 0 );
	}
}

static void mp_callback( const struct libvlc_event_t *evt, void *data )
{
	consumer_libvlc self = data;
	assert( self != NULL );

	switch ( evt->type )
	{
		case libvlc_MediaPlayerStopped:
			self->running = 0;
			break;

		default:
			assert( 0 );
	}
}

static int consumer_start( mlt_consumer parent )
{
	int err;

	consumer_libvlc self = parent->child;
	assert( self != NULL );

	mlt_properties properties = MLT_CONSUMER_PROPERTIES( self->parent );


	if ( consumer_is_stopped( parent ) )
	{
		// Free all previous resources
		if ( self->media_player )
		{
			libvlc_media_player_release( self->media_player );
			self->media_player = NULL;
		}
		if ( self->media )
		{
			libvlc_media_release( self->media );
			self->media = NULL;
		}

		// Apply properties to new media
		setup_vlc( self );
		self->media_player = libvlc_media_player_new_from_media( self->media );
		assert( self->media_player != NULL );

		// Set window output if we're using it
		if ( self->output_to_window )
		{
			err = setup_vlc_window( self );

			if ( err )
			{
				char error_msg[] = "Wrong window_type and/or output_dst parameters supplied.\n";
				mlt_log_error( MLT_CONSUMER_SERVICE( self->parent ), error_msg );
				mlt_events_fire( properties, "consumer-fatal-error", NULL );
				return err;
			}
		}

		// Catch media_player stop
		self->mp_manager = libvlc_media_player_event_manager( self->media_player );
		assert( self->mp_manager != NULL );
		libvlc_event_attach( self->mp_manager, libvlc_MediaPlayerStopped, &mp_callback, self );

		// Run media player
		self->running = 1;
		err = libvlc_media_player_play( self->media_player );

		// If we failed to play, we're not running
		if ( err ) {
			self->running = 0;
		}

		return err;
	}
	return 1;
}

static int consumer_stop( mlt_consumer parent )
{
	consumer_libvlc self = parent->child;
	assert( self != NULL );

	if ( self->media_player )
	{
		self->running = 0;
		libvlc_media_player_stop( self->media_player );
	}

	// Reset pts counters
	self->latest_video_pts = 0;
	self->latest_audio_pts = 0;

	return 0;
}

static int consumer_is_stopped( mlt_consumer parent )
{
	consumer_libvlc self = parent->child;
	assert( self != NULL );

	if ( self->media_player )
	{
		return !self->running;
	}

	return 1;
}

static void consumer_purge( mlt_consumer parent )
{
	// We do nothing here, we purge on stop()
}

static void consumer_close( mlt_consumer parent )
{
	if ( parent == NULL )
	{
		return;
	}

	consumer_libvlc self = parent->child;

	if ( self != NULL )
	{
		consumer_stop( parent );

		if ( self->media_player )
			libvlc_media_player_release( self->media_player );

		if ( self->media )
			libvlc_media_release( self->media );

		if ( self->vlc )
			libvlc_release( self->vlc );

		pthread_mutex_destroy( &self->queue_mutex );
		free( self );
	}

	parent->close = NULL;
	mlt_consumer_close( parent );
}
