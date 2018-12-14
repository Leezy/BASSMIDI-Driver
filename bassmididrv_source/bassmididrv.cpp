﻿/*
    BASSMIDI Driver
*/

#define STRICT

#if __DMC__
unsigned long _beginthreadex( void *security, unsigned stack_size,
		unsigned ( __stdcall *start_address )( void * ), void *arglist,
		unsigned initflag, unsigned *thrdaddr );
void _endthreadex( unsigned retval );
#endif

#define _CRT_SECURE_CPP_OVERLOAD_STANDARD_NAMES 1 
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <process.h>
#include <Shlwapi.h>
#include <tchar.h>
#include <mmddk.h>

#include <vector>

#define BASSDEF(f) (WINAPI *f)	// define the BASS/BASSMIDI functions as pointers
#define BASSMIDIDEF(f) (WINAPI *f)	
#define BASSWASAPIDEF(f) (WINAPI *f)
#define LOADBASSFUNCTION(f) *((void**)&f)=GetProcAddress(bass,#f)
#define LOADBASSMIDIFUNCTION(f) *((void**)&f)=GetProcAddress(bassmidi,#f)
#define LOADBASSWASAPIFUNCTION(f) *((void**)&f)=GetProcAddress(basswasapi,#f)
#include <bass.h>
#include <bassmidi.h>
#include <basswasapi.h>



#define MAX_DRIVERS 2
#define MAX_CLIENTS 1 // Per driver

#define SAMPLES_PER_FRAME 88 * 2
#define FRAMES_WASAPI 30
#define FRAMES_DSOUND 60
#define SAMPLE_RATE_DEFAULT 44100
#define LONGMSG_MAXSIZE	65535

static DWORD SAMPLE_RATE_USED = SAMPLE_RATE_DEFAULT;

struct Driver_Client {
	int allocated;
	DWORD_PTR instance;
	DWORD flags;
	DWORD_PTR callback;
};

//Note: drivers[0] is not used (See OnDriverOpen).
struct Driver {
	int open;
	int clientCount;
	HDRVR hdrvr;
	struct Driver_Client clients[MAX_CLIENTS];
} drivers[MAX_DRIVERS+1];

static int driverCount=0;

static volatile int OpenCount = 0;
static volatile int modm_closed = 1;

static CRITICAL_SECTION mim_section;
static CRITICAL_SECTION bass_section;
static volatile int stop_thread = 0;
static volatile int reset_synth[2] = {0, 0};
static HANDLE hCalcThread = NULL;
static DWORD processPriority;
static HANDLE load_sfevent = NULL; 

static DWORD wasapi_frames = 30; // default
static DWORD dsound_frames = 60; // default

static unsigned int font_count[2] = { 0, 0 };
static HSOUNDFONT * hFonts[2] = { NULL, NULL };
static HSTREAM hStream[2] = {0, 0};
static HSTREAM hStOutput = 0;

static BOOL sound_out_float = FALSE;
static int wasapi_bits = 16;
static float sound_out_volume_float = 1.0;
static int sound_out_volume_int = 0x1000;

static HINSTANCE bass = 0;			// bass handle
static HINSTANCE bassmidi = 0;			// bassmidi handle
static HINSTANCE basswasapi = 0;        // basswasapi handle
//TODO: Can be done with: HMODULE GetDriverModuleHandle(HDRVR hdrvr);  (once DRV_OPEN has been called)
static HINSTANCE hinst = NULL;             //main DLL handle

static void DoStopClient();

BOOL IsWin8OrNewer()
{
	OSVERSIONINFOEX osvi;
	BOOL bOsVersionInfoEx;
	ZeroMemory(&osvi, sizeof(OSVERSIONINFOEX));
	osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
	bOsVersionInfoEx = GetVersionEx((OSVERSIONINFO*)&osvi);
	if (bOsVersionInfoEx == FALSE) return FALSE;
	if (VER_PLATFORM_WIN32_NT == osvi.dwPlatformId &&
		(osvi.dwMajorVersion > 6 ||
		(osvi.dwMajorVersion == 6 && osvi.dwMinorVersion > 1)))
		return TRUE;
	return FALSE;
}

class message_window
{
	HWND m_hWnd;
	ATOM class_atom;

public:
	message_window() {
		static const TCHAR * class_name = _T("bassmididrv message window");
		WNDCLASSEX cls = { 0 };
		cls.cbSize = sizeof(cls);
		cls.lpfnWndProc = DefWindowProc;
		cls.hInstance = hinst;
		cls.lpszClassName = class_name;
		class_atom = RegisterClassEx( &cls );
		if ( class_atom ) {
			m_hWnd = CreateWindowEx( 0, (LPCTSTR) class_atom, _T("bassmididrv"), 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hinst, NULL );
		} else {
			m_hWnd = NULL;
		}
	}

	~message_window()
	{
		if ( IsWindow( m_hWnd ) ) DestroyWindow( m_hWnd );
		if ( class_atom ) UnregisterClass( (LPCTSTR) class_atom, hinst );
	}

	HWND get_hwnd() const { return m_hWnd; }
};

message_window * g_msgwnd = NULL;

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved ){
	if (fdwReason == DLL_PROCESS_ATTACH){
	    hinst = hinstDLL;
		DisableThreadLibraryCalls(hinstDLL);
		g_msgwnd = new message_window;
	}else if(fdwReason == DLL_PROCESS_DETACH){
		;
		DoStopClient();
		delete g_msgwnd;
	}
	return TRUE;    
}

std::vector<HSOUNDFONT> _soundFonts[2];
std::vector<BASS_MIDI_FONTEX> presetList[2];

static void FreeFonts(UINT uDeviceID)
{
	unsigned i;
	if ( _soundFonts[uDeviceID].size() )
	{
		for (auto it = _soundFonts[uDeviceID].begin(); it != _soundFonts[uDeviceID].end(); ++it)
		{
			BASS_MIDI_FontFree( *it );
		}
		_soundFonts[uDeviceID].resize(0);
		presetList[uDeviceID].resize(0);
	}
}

static bool load_font_item(unsigned uDeviceID, const TCHAR * in_path)
{
	const DWORD bass_flags =
#ifdef UNICODE
		BASS_UNICODE
#else
		0
#endif
		;

	const TCHAR * ext = _T("");
	const TCHAR * dot = _tcsrchr(in_path, _T('.'));
	if (dot != 0) ext = dot + 1;
	if (!_tcsicmp(ext, _T("sf2"))
		|| !_tcsicmp(ext, _T("sf2pack"))
		)
	{
		HSOUNDFONT font = BASS_MIDI_FontInit(in_path, bass_flags);
		if (!font)
		{
			return false;
		}
		_soundFonts[uDeviceID].push_back(font);
		BASS_MIDI_FONTEX fex = { font, -1, -1, -1, 0, 0 };
		presetList[uDeviceID].push_back(fex);
		return true;
	}
	else if (!_tcsicmp(ext, _T("sflist")))
	{
		FILE * fl = _tfopen(in_path, _T("r, ccs=UTF-8"));
		if (fl)
		{
			TCHAR path[32768], temp[32768];
			TCHAR name[32768];
			TCHAR *nameptr;
			const TCHAR * slash = _tcsrchr(in_path, _T('\\'));
			if (slash != 0) _tcsncpy(path, in_path, slash - in_path + 1);
			while (!feof(fl))
			{
				std::vector<BASS_MIDI_FONTEX> presets;

				if (!_fgetts(name, 32767, fl)) break;
				name[32767] = 0;
				TCHAR * cr = _tcschr(name, _T('\n'));
				if (cr) *cr = 0;
				cr = _tcschr(name, _T('\r'));
				if (cr) *cr = 0;
				cr = _tcschr(name, '|');
				if (cr)
				{
					bool valid = true;
					TCHAR *endchr;
					nameptr = cr + 1;
					*cr = 0;
					cr = name;
					while (*cr && valid)
					{
						switch (*cr++)
						{
						case 'p':
						{
							// patch override - "p[db#,]dp#=[sb#,]sp#" ex. "p0,5=0,1"
							// may be used once per preset group
							long dbank = 0;
							long dpreset = _tcstol(cr, &endchr, 10);
							if (endchr == cr)
							{
								valid = false;
								break;
							}
							if (*endchr == ',')
							{
								dbank = dpreset;
								cr = endchr + 1;
								dpreset = _tcstol(cr, &endchr, 10);
								if (endchr == cr)
								{
									valid = false;
									break;
								}
							}
							if (*endchr != '=')
							{
								valid = false;
								break;
							}
							cr = endchr + 1;
							long sbank = -1;
							long spreset = _tcstol(cr, &endchr, 10);
							if (endchr == cr)
							{
								valid = false;
								break;
							}
							if (*endchr == ',')
							{
								sbank = spreset;
								cr = endchr + 1;
								spreset = _tcstol(cr, &endchr, 10);
								if (endchr == cr)
								{
									valid = false;
									break;
								}
							}
							if (*endchr && *endchr != ';' && *endchr != '&')
							{
								valid = false;
								break;
							}
							cr = endchr;
							BASS_MIDI_FONTEX fex = { 0, (int)spreset, (int)sbank, (int)dpreset, (int)dbank, 0 };
							presets.push_back(fex);
						}
						break;

						case '&':
						{
						}
						break;

						case ';':
							// separates preset items
							break;

						default:
							// invalid command character
							valid = false;
							break;
						}
					}
					if (!valid)
					{
						presets.clear();
						BASS_MIDI_FONTEX fex = { 0, -1, -1, -1, 0, 0 };
						presets.push_back(fex);
					}
				}
				else
				{
					BASS_MIDI_FONTEX fex = { 0, -1, -1, -1, 0, 0 };
					presets.push_back(fex);
					nameptr = name;
				}
				if ((isalpha(nameptr[0]) && nameptr[1] == _T(':')) || nameptr[0] == '\\')
				{
					_tcscpy(temp, nameptr);
				}
				else
				{
					_tcscpy(temp, path);
					_tcscat(temp, nameptr);
				}
				HSOUNDFONT font = BASS_MIDI_FontInit(temp, bass_flags);
				if (!font)
				{
					fclose(fl);
					return false;
				}
				for (auto it = presets.begin(); it != presets.end(); ++it)
				{
					it->font = font;
					presetList[uDeviceID].push_back(*it);
				}
				_soundFonts[uDeviceID].push_back(font);
			}
			fclose(fl);
			return true;
		}
	}
	return false;
}

void LoadFonts(UINT uDeviceID, const TCHAR * name)
{
	FreeFonts(uDeviceID);

	if (name && *name)
	{
		const TCHAR * ext = _tcsrchr( name, _T('.') );
		if ( ext ) ext++;
		if ( !_tcsicmp( ext, _T("sf2") ) || !_tcsicmp( ext, _T("sf2pack") ) )
		{
			if (!load_font_item(uDeviceID, name))
			{
				FreeFonts(uDeviceID);
				return;
			}
		}
		else if ( !_tcsicmp( ext, _T("sflist") ) )
		{
			if (!load_font_item(uDeviceID, name))
			{
				FreeFonts(uDeviceID);
				return;
			}
		}

		std::vector< BASS_MIDI_FONTEX > fonts;
		for (unsigned long i = 0, j = presetList[uDeviceID].size(); i < j; ++i)
		{
			fonts.push_back(presetList[uDeviceID][j - i - 1]);
		}
		BASS_MIDI_StreamSetFonts(hStream[uDeviceID], &fonts[0], (unsigned int)fonts.size() | BASS_MIDI_FONT_EX);
	}
}

LRESULT DoDriverLoad() {
	//The DRV_LOAD message is always the first message that a device driver receives. 
	//Notifies the driver that it has been loaded. The driver should make sure that any hardware and supporting drivers it needs to function properly are present.
	//TODO: Check about existence of bass and bassmidi dlls.
	memset(drivers, 0, sizeof(drivers));
	driverCount = 0;
	return DRV_OK;
}

LRESULT DoDriverOpen(HDRVR hdrvr, LPCWSTR driverName, LONG lParam) {

/*
Remarks

If the driver returns a nonzero value, the system uses that value as the driver identifier (the dwDriverId parameter)
in messages it subsequently sends to the driver instance. The driver can return any type of value as the identifier.
For example, some drivers return memory addresses that point to instance-specific information. Using this method of 
specifying identifiers for a driver instance gives the drivers ready access to the information while they are processing messages.
*/

/*
When the driver's DriverProc function receives a
DRV_OPEN message, it should:
1. Allocate memory space for a structure instance.
2. Add the structure instance to the linked list.
3. Store instance data in the new list entry.
4. Specify the entry's number or address as the return value for the DriverProc function.
Subsequent calls to DriverProc will include the list entry's identifier as its dwDriverID
argument
*/
	int driverNum;
	if (driverCount == MAX_DRIVERS) {
		return 0;
	} else {
		for (driverNum = 1; driverNum < MAX_DRIVERS; driverNum++) {
			if (!drivers[driverNum].open) {
				break;
			}
		}
		if (driverNum == MAX_DRIVERS) {
			return 0;
		}
	}
	drivers[driverNum].open = 1;
	drivers[driverNum].clientCount = 0;
	drivers[driverNum].hdrvr = hdrvr;
	driverCount++;
	return driverNum;
}

LRESULT DoDriverClose(DWORD_PTR dwDriverId, HDRVR hdrvr, LONG lParam1, LONG lParam2) {
	int i;
	for (i = 0; i < MAX_DRIVERS; i++) {
		if (drivers[i].open && drivers[i].hdrvr == hdrvr) {
			drivers[i].open = 0;
			--driverCount;
			return DRV_OK;
		}
	}
	return DRV_CANCEL;
}

LRESULT DoDriverConfigure(DWORD_PTR dwDriverId, HDRVR hdrvr, HWND parent, DRVCONFIGINFO* configInfo) {
	return DRV_CANCEL;
}

/* INFO Installable Driver Reference: http://msdn.microsoft.com/en-us/library/ms709328%28v=vs.85%29.aspx */
/* The original header is LONG DriverProc(DWORD dwDriverId, HDRVR hdrvr, UINT msg, LONG lParam1, LONG lParam2);
but that does not support 64bit. See declaration of DefDriverProc to see where the values come from.
*/
STDAPI_(LRESULT) DriverProc(DWORD_PTR dwDriverId, HDRVR hdrvr, UINT uMsg, LPARAM lParam1, LPARAM lParam2)
{
	switch(uMsg) {
/* Seems this is only for kernel mode drivers
	case DRV_INSTALL:
		return DoDriverInstall(dwDriverId, hdrvr, static_cast<DRVCONFIGINFO*>(lParam2));
	case DRV_REMOVE:
		DoDriverRemove(dwDriverId, hdrvr);
		return DRV_OK;
*/
	case DRV_QUERYCONFIGURE:
		//TODO: Until it doesn't have a configuration window, it should return 0.
		return DRV_CANCEL;
	case DRV_CONFIGURE:
		return DoDriverConfigure(dwDriverId, hdrvr, reinterpret_cast<HWND>(lParam1), reinterpret_cast<DRVCONFIGINFO*>(lParam2));

/* TODO: Study this. It has implications: 
		Calling OpenDriver, described in the Win32 SDK. This function calls SendDriverMessage to
		send DRV_LOAD and DRV_ENABLE messages only if the driver has not been previously loaded,
		and then to send DRV_OPEN.
		· Calling CloseDriver, described in the Win32 SDK. This function calls SendDriverMessage to
		send DRV_CLOSE and, if there are no other open instances of the driver, to also send
		DRV_DISABLE and DRV_FREE.
*/
	case DRV_LOAD:
		return DoDriverLoad();
	case DRV_FREE:
		//The DRV_FREE message is always the last message that a device driver receives. 
		//Notifies the driver that it is being removed from memory. The driver should free any memory and other system resources that it has allocated.
		return DRV_OK;
	case DRV_OPEN:
		return DoDriverOpen(hdrvr, reinterpret_cast<LPCWSTR>(lParam1), static_cast<LONG>(lParam2));
	case DRV_CLOSE:
		return DoDriverClose(dwDriverId, hdrvr, static_cast<LONG>(lParam1), static_cast<LONG>(lParam2));
	default:
		return DefDriverProc(dwDriverId, hdrvr, uMsg, lParam1, lParam2);
	}
}

HRESULT modGetCaps(UINT uDeviceID, MIDIOUTCAPS* capsPtr, DWORD capsSize) {
	MIDIOUTCAPSA * myCapsA;
	MIDIOUTCAPSW * myCapsW;
	MIDIOUTCAPS2A * myCaps2A;
	MIDIOUTCAPS2W * myCaps2W;
	CHAR synthName[] = "BASSMIDI Driver";
	WCHAR synthNameW[] = L"BASSMIDI Driver";

	CHAR synthPortA[] = " (port A)\0";
	WCHAR synthPortAW[] = L" (port A)\0";

	CHAR synthPortB[] = " (port B)\0";
	WCHAR synthPortBW[] = L" (port B)\0";


	switch (capsSize) {
	case (sizeof(MIDIOUTCAPSA)):
		myCapsA = (MIDIOUTCAPSA *)capsPtr;
		myCapsA->wMid = 0xffff;
		myCapsA->wPid = 0xffff;
		memcpy(myCapsA->szPname, synthName, strlen(synthName));
		memcpy(myCapsA->szPname + strlen(synthName), uDeviceID ? synthPortB : synthPortA, sizeof(synthPortA));
		myCapsA->wTechnology = MOD_MIDIPORT;
		myCapsA->vDriverVersion = 0x0090;
		myCapsA->wVoices = 0;
		myCapsA->wNotes = 0;
		myCapsA->wChannelMask = 0xffff;
		myCapsA->dwSupport = MIDICAPS_VOLUME;
		return MMSYSERR_NOERROR;

	case (sizeof(MIDIOUTCAPSW)):
		myCapsW = (MIDIOUTCAPSW *)capsPtr;
		myCapsW->wMid = 0xffff;
		myCapsW->wPid = 0xffff;
		memcpy(myCapsW->szPname, synthNameW, wcslen(synthNameW) * sizeof(wchar_t));
		memcpy(myCapsW->szPname + wcslen(synthNameW), uDeviceID ? synthPortBW : synthPortAW, sizeof(synthPortAW));
		myCapsW->wTechnology = MOD_MIDIPORT;
		myCapsW->vDriverVersion = 0x0090;
		myCapsW->wVoices = 0;
		myCapsW->wNotes = 0;
		myCapsW->wChannelMask = 0xffff;
		myCapsW->dwSupport = MIDICAPS_VOLUME;
		return MMSYSERR_NOERROR;

	case (sizeof(MIDIOUTCAPS2A)):
		myCaps2A = (MIDIOUTCAPS2A *)capsPtr;
		myCaps2A->wMid = 0xffff;
		myCaps2A->wPid = 0xffff;
		memcpy(myCaps2A->szPname, synthName, strlen(synthName));
		memcpy(myCaps2A->szPname + strlen(synthName), uDeviceID ? synthPortB : synthPortA, sizeof(synthPortA));
		myCaps2A->wTechnology = MOD_MIDIPORT;
		myCaps2A->vDriverVersion = 0x0090;
		myCaps2A->wVoices = 0;
		myCaps2A->wNotes = 0;
		myCaps2A->wChannelMask = 0xffff;
		myCaps2A->dwSupport = MIDICAPS_VOLUME;
		return MMSYSERR_NOERROR;

	case (sizeof(MIDIOUTCAPS2W)):
		myCaps2W = (MIDIOUTCAPS2W *)capsPtr;
		myCaps2W->wMid = 0xffff;
		myCaps2W->wPid = 0xffff;
		memcpy(myCaps2W->szPname, synthNameW, wcslen(synthNameW) * sizeof(wchar_t));
		memcpy(myCaps2W->szPname + wcslen(synthNameW), uDeviceID ? synthPortBW : synthPortAW, sizeof(synthPortAW));
		myCaps2W->wTechnology = MOD_MIDIPORT;
		myCaps2W->vDriverVersion = 0x0090;
		myCaps2W->wVoices = 0;
		myCaps2W->wNotes = 0;
		myCaps2W->wChannelMask = 0xffff;
		myCaps2W->dwSupport = MIDICAPS_VOLUME;
		return MMSYSERR_NOERROR;

	default:
		return MMSYSERR_ERROR;
	}
}


struct evbuf_t{
	UINT uDeviceID;
	UINT   uMsg;
	DWORD_PTR	dwParam1;
	DWORD_PTR	dwParam2;
};
#define EVBUFF_SIZE 2048
static struct evbuf_t evbuf[EVBUFF_SIZE];
static volatile LONG evbwpoint=0;
static volatile LONG evbrpoint=0;
static volatile LONG evbcount=0;
static UINT evbsysexpoint;

static int bmsyn_buf_check(void){
	int retval;
	EnterCriticalSection(&mim_section);
	retval = evbcount;
	LeaveCriticalSection(&mim_section);
	return retval;
}

static int bmsyn_play_some_data(void){
	int played;
	UINT evbpoint;
	UINT evcode;
	UINT evtype;
	UINT exlen;
	
	played=0;
		if( !bmsyn_buf_check() ){ 
			played=~0;
			return played;
		}
		do{
			EnterCriticalSection(&mim_section);
			evbpoint=evbrpoint;
			if (++evbrpoint >= EVBUFF_SIZE)
					evbrpoint -= EVBUFF_SIZE;		
			evbuf_t tempevent = evbuf[evbpoint];
			LeaveCriticalSection(&mim_section);
			
			switch (tempevent.uMsg) {
			case MODM_DATA:
				evcode = tempevent.dwParam1;
				evtype = evcode & 0xF0;
				exlen = ( evcode >= 0xF8 && evcode <= 0xFF ) ? 1 : (( evtype == 0xC0 || evtype == 0xD0 ) ? 2 : 3);
				BASS_MIDI_StreamEvents( hStream[tempevent.uDeviceID], BASS_MIDI_EVENTS_RAW, &tempevent.dwParam1, exlen );
				break;
			}
		} while (InterlockedDecrement(&evbcount));
	return played;
}

static void load_settings();
static int check_preload();

DWORD CALLBACK StreamProc(HSTREAM handle, void *buffer, DWORD length, void *user)
{
	DWORD bytes_done = 0;
	if (sound_out_float) {
		float sound_buffer[2][SAMPLES_PER_FRAME];
		length /= sizeof(float);
		while (length) {
			int samples_todo = length > SAMPLES_PER_FRAME ? SAMPLES_PER_FRAME : length;
			EnterCriticalSection(&bass_section);

			if (reset_synth[ 0 ] != 0){
				reset_synth[ 0 ] = 0;
				load_settings();
				BASS_MIDI_StreamEvent( hStream[ 0 ], 0, MIDI_EVENT_SYSTEM, MIDI_SYSTEM_DEFAULT );
				if (check_preload())
					BASS_MIDI_StreamLoadSamples( hStream[0] );
			}
			if (reset_synth[ 1 ] != 0){
				reset_synth[ 1 ] = 0;
				load_settings();
				BASS_MIDI_StreamEvent( hStream[ 1 ], 0, MIDI_EVENT_SYSTEM, MIDI_SYSTEM_DEFAULT );
				if (check_preload())
					BASS_MIDI_StreamLoadSamples( hStream[1] );
			}

			bmsyn_play_some_data();
			int decoded = BASS_ChannelGetData( hStream[ 0 ], sound_buffer[0], BASS_DATA_FLOAT + samples_todo * sizeof(float) );
			int decoded2 = BASS_ChannelGetData( hStream[ 1 ], sound_buffer[1], BASS_DATA_FLOAT + samples_todo * sizeof(float) );
			LeaveCriticalSection(&bass_section);
			if ( decoded < 0 || decoded2 < 0 ) return bytes_done;
			else {
				assert( decoded == decoded2 );
				for ( unsigned i = 0, j = decoded / sizeof(float); i < j; i++ ) {
					float sample = sound_buffer[ 0 ][ i ] + sound_buffer[ 1 ][ i ];
					sample *= sound_out_volume_float;
					sound_buffer[ 0 ][ i ] = sample;
				}
				memcpy(buffer, sound_buffer[ 0 ], decoded);
				buffer = (void *)(((unsigned char *)buffer) + decoded);
				bytes_done += decoded;
			}
			length -= decoded / sizeof(float);
		}
	} else {
		short sound_buffer[2][SAMPLES_PER_FRAME];
		length /= sizeof(short);
		while (length) {
			int samples_todo = length > SAMPLES_PER_FRAME ? SAMPLES_PER_FRAME : length;
			EnterCriticalSection(&bass_section);
			bmsyn_play_some_data();
			int decoded = BASS_ChannelGetData( hStream[ 0 ], sound_buffer[0], samples_todo * sizeof(short) );
			int decoded2 = BASS_ChannelGetData( hStream[ 1 ], sound_buffer[1], samples_todo * sizeof(short) );
			LeaveCriticalSection(&bass_section);
			if ( decoded < 0 || decoded2 < 0 ) return bytes_done;
			else {
				assert( decoded == decoded2 );
				for ( unsigned i = 0, j = decoded / sizeof(short); i < j; i++ ) {
					int sample = sound_buffer[ 0 ][ i ] + sound_buffer[ 1 ][ i ];
					sample = ( sample * sound_out_volume_int ) >> 12;
					if ( ( sample + 0x8000 ) & 0xFFFF0000 ) sample = 0x7FFF ^ ( sample >> 31 );
					sound_buffer[ 0 ][ i ] = sample;
				}
				memcpy(buffer, sound_buffer[ 0 ], decoded);
				buffer = (void *)(((unsigned char *)buffer) + decoded);
				bytes_done += decoded;
			}
			length -= decoded / sizeof(short);
		}
	}
	return bytes_done;
}

DWORD CALLBACK WasapiProc(void *buffer, DWORD length, void *user)
{
	if (sound_out_float || wasapi_bits == 16)
		return StreamProc(NULL, buffer, length, 0);
	else
	{
		int bytes_per_sample = wasapi_bits / 8;
		int bytes_done = 0;
		while (length)
		{
			unsigned short sample_buffer[SAMPLES_PER_FRAME];
			int length_todo = (length / bytes_per_sample);
			if (length_todo > SAMPLES_PER_FRAME) length_todo = SAMPLES_PER_FRAME;
			int bytes_done_this = StreamProc(NULL, sample_buffer, length_todo * 4, 0);
			if (bytes_done_this <= 0) return bytes_done;
			if (wasapi_bits == 32)
			{
				unsigned int * out = (unsigned int *) buffer;
				for (int i = 0; i < bytes_done_this; i += 2)
				{
					*out++ = sample_buffer[i / 2] << 16;
				}
				buffer = out;
			}
			else if (wasapi_bits == 24)
			{
				unsigned char * out = (unsigned char *) buffer;
				for (int i = 0; i < bytes_done_this; i += 2)
				{
					int sample = sample_buffer[i / 2];
					*out++ = 0;
					*out++ = sample & 0xFF;
					*out++ = (sample >> 8) & 0xFF;
				}
				buffer = out;
			}
			else if (wasapi_bits == 8)
			{
				unsigned char * out = (unsigned char *) buffer;
				for (int i = 0; i < bytes_done_this; i += 2)
				{
					*out++ = (sample_buffer[i / 2] >> 8) & 0xFF;
				}
				buffer = out;
			}
			bytes_done += (bytes_done_this / 2) * bytes_per_sample;
			length -= (bytes_done_this / 2) * bytes_per_sample;
		}
		return bytes_done;
	}
}

void load_settings()
{
	int config_volume;
	HKEY hKey;
	long lResult;
	DWORD dwType=REG_DWORD;
	DWORD dwSize=sizeof(DWORD);
	lResult = RegOpenKeyEx(HKEY_LOCAL_MACHINE, L"Software\\BASSMIDI Driver", 0, KEY_READ | KEY_WOW64_32KEY, &hKey);
	RegQueryValueEx(hKey, L"volume", NULL, &dwType,(LPBYTE)&config_volume, &dwSize);
	RegQueryValueEx(hKey, L"dbuflen", NULL, &dwType,(LPBYTE)&dsound_frames, &dwSize);
	RegCloseKey( hKey);
	sound_out_volume_float = (float)config_volume / 10000.0f;
	sound_out_volume_int = (int)(sound_out_volume_float * (float)0x1000);
}

static int check_sinc()
{
	DWORD sinc = 0;
	HKEY hKey;
	long lResult;
	DWORD dwType=REG_DWORD;
	DWORD dwSize=sizeof(DWORD);
	lResult = RegOpenKeyEx(HKEY_LOCAL_MACHINE, L"Software\\BASSMIDI Driver", 0, KEY_READ | KEY_WOW64_32KEY, &hKey);
	RegQueryValueEx(hKey, L"sinc", NULL, &dwType,(LPBYTE)&sinc, &dwSize);
	RegCloseKey( hKey);
	return sinc;
}

static int check_preload()
{
	DWORD preload = 1;
	HKEY hKey;
	long lResult;
	DWORD dwType = REG_DWORD;
	DWORD dwSize = sizeof(DWORD);
	lResult = RegOpenKeyEx(HKEY_LOCAL_MACHINE, L"Software\\BASSMIDI Driver", 0, KEY_READ | KEY_WOW64_32KEY, &hKey);
	RegQueryValueEx(hKey, L"preload", NULL, &dwType, (LPBYTE)&preload, &dwSize);
	RegCloseKey(hKey);
	return preload;
}

BOOL load_bassfuncs()
{
		TCHAR installpath[MAX_PATH] = {0};
		TCHAR basspath[MAX_PATH] = {0};
		TCHAR bassmidipath[MAX_PATH] = {0};
		TCHAR basswasapipath[MAX_PATH] = {0};
		TCHAR pluginpath[MAX_PATH] = {0};
		WIN32_FIND_DATA fd;
		HANDLE fh;
		int installpathlength;
		
		GetModuleFileName(hinst, installpath, MAX_PATH);
		PathRemoveFileSpec(installpath);

		lstrcat(basspath,installpath);
		lstrcat(basspath,L"\\bass.dll");
		if (!(bass=LoadLibrary(basspath))) {
			OutputDebugString(L"Failed to load BASS DLL!");
			return FALSE;
		}
		lstrcat(bassmidipath,installpath);
		lstrcat(bassmidipath,L"\\bassmidi.dll");
		if (!(bassmidi=LoadLibrary(bassmidipath))) {
			OutputDebugString(L"Failed to load BASSMIDI DLL!");
			return FALSE;
		}
		lstrcat(basswasapipath,installpath);
		lstrcat(basswasapipath,L"\\basswasapi.dll");
		basswasapi=LoadLibrary(basswasapipath);
		/* "load" all the BASS functions that are to be used */
		OutputDebugString(L"Loading BASS functions....");
		LOADBASSFUNCTION(BASS_ErrorGetCode);
		LOADBASSFUNCTION(BASS_SetConfig);
		LOADBASSFUNCTION(BASS_Init);
		LOADBASSFUNCTION(BASS_Free);
		LOADBASSFUNCTION(BASS_GetInfo);
		LOADBASSFUNCTION(BASS_StreamCreate);
		LOADBASSFUNCTION(BASS_StreamFree);
		LOADBASSFUNCTION(BASS_PluginLoad);
		LOADBASSFUNCTION(BASS_ChannelGetData);
		LOADBASSFUNCTION(BASS_ChannelPlay);
		LOADBASSFUNCTION(BASS_ChannelStop);
		LOADBASSMIDIFUNCTION(BASS_MIDI_StreamCreate);
		LOADBASSMIDIFUNCTION(BASS_MIDI_FontInit);
		LOADBASSMIDIFUNCTION(BASS_MIDI_FontFree);
		LOADBASSMIDIFUNCTION(BASS_MIDI_StreamSetFonts);
		LOADBASSMIDIFUNCTION(BASS_MIDI_StreamEvents);
		LOADBASSMIDIFUNCTION(BASS_MIDI_StreamEvent);
		LOADBASSMIDIFUNCTION(BASS_MIDI_StreamLoadSamples);

		if (basswasapi) {
			LOADBASSWASAPIFUNCTION(BASS_WASAPI_Init);
			LOADBASSWASAPIFUNCTION(BASS_WASAPI_Free);
			LOADBASSWASAPIFUNCTION(BASS_WASAPI_Start);
			LOADBASSWASAPIFUNCTION(BASS_WASAPI_Stop);
			LOADBASSWASAPIFUNCTION(BASS_WASAPI_GetInfo);
		}

		installpathlength=lstrlen(installpath)+1;
		lstrcat(pluginpath,installpath);
		lstrcat(pluginpath,L"\\bass*.dll");
		fh=FindFirstFile(pluginpath,&fd);
		if (fh!=INVALID_HANDLE_VALUE) {
			do {
				HPLUGIN plug;
				pluginpath[installpathlength]=0;
				lstrcat(pluginpath,fd.cFileName);
				plug=BASS_PluginLoad((char*)pluginpath,BASS_UNICODE);
			} while (FindNextFile(fh,&fd));
			FindClose(fh);
		}

		return TRUE;
}

BOOL IsVistaOrNewer(){
	OSVERSIONINFOEX osvi;
	BOOL bOsVersionInfoEx;
	ZeroMemory(&osvi, sizeof(OSVERSIONINFOEX));
	osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
	bOsVersionInfoEx = GetVersionEx((OSVERSIONINFO*) &osvi);
	if(bOsVersionInfoEx == FALSE) return FALSE;
	if ( VER_PLATFORM_WIN32_NT==osvi.dwPlatformId && 
		osvi.dwMajorVersion > 5 )
		return TRUE;
	return FALSE;
}

unsigned __stdcall threadfunc(LPVOID lpV){
	unsigned i;
	int opend=0;
	TCHAR config[MAX_PATH];
	TCHAR configb[MAX_PATH];
	BASS_MIDI_FONT * mf;
	BASS_WASAPI_INFO winfo;
	int len;

	while(opend == 0 && stop_thread == 0) {
		Sleep(100);
		load_settings();
		load_bassfuncs();
		BASS_SetConfig(BASS_CONFIG_MIDI_VOICES, 1000);
		BASS_SetConfig(BASS_CONFIG_MIDI_COMPACT, !check_preload());
		BASS_SetConfig(BASS_CONFIG_UPDATEPERIOD, 5);
		if ( BASS_Init( basswasapi ? 0 : -1, SAMPLE_RATE_USED, BASS_DEVICE_LATENCY, NULL, NULL ) ) {
			if (basswasapi) {
				if (!BASS_WASAPI_Init(-1, 0, 2, BASS_WASAPI_EVENT, (float)wasapi_frames * 0.001, 0.005, WasapiProc, 0 ))
					continue;
				if (!BASS_WASAPI_GetInfo(&winfo))
				{
					BASS_WASAPI_Free();
					continue;
				}
				SAMPLE_RATE_USED = winfo.freq;
				sound_out_float = FALSE;
				switch (winfo.format) {
				case BASS_WASAPI_FORMAT_8BIT:
					wasapi_bits = 8;
					break;

				case BASS_WASAPI_FORMAT_16BIT:
					wasapi_bits = 16;
					break;

				case BASS_WASAPI_FORMAT_24BIT:
					wasapi_bits = 24;
					break;

				case BASS_WASAPI_FORMAT_32BIT:
					wasapi_bits = 32;
					break;

				case BASS_WASAPI_FORMAT_FLOAT:
					sound_out_float = TRUE;
					break;
				}
			}
			else {
				hStOutput = BASS_StreamCreate( SAMPLE_RATE_USED, 2, ( sound_out_float ? BASS_SAMPLE_FLOAT : 0 ), StreamProc, 0 );
				if (!hStOutput) continue;
			}

			hStream[0] = BASS_MIDI_StreamCreate( 16, BASS_STREAM_DECODE | ( sound_out_float ? BASS_SAMPLE_FLOAT : 0 ) | (check_sinc()?BASS_MIDI_SINCINTER: 0), SAMPLE_RATE_USED );
			if (!hStream[0]) {
				BASS_StreamFree(hStOutput);
				hStOutput = 0;
				continue;
			}
			hStream[1] = BASS_MIDI_StreamCreate( 16, BASS_STREAM_DECODE | ( sound_out_float ? BASS_SAMPLE_FLOAT : 0 ) | (check_sinc()?BASS_MIDI_SINCINTER: 0), SAMPLE_RATE_USED );
			if (!hStream[1]) {
				BASS_StreamFree(hStream[0]);
				hStream[0] = 0;
				BASS_StreamFree(hStOutput);
				hStOutput = 0;
				continue;
			}
			BASS_MIDI_StreamEvent( hStream[0], 0, MIDI_EVENT_SYSTEM, MIDI_SYSTEM_DEFAULT );
			BASS_MIDI_StreamEvent( hStream[1], 0, MIDI_EVENT_SYSTEM, MIDI_SYSTEM_DEFAULT );
		
			if (GetWindowsDirectory(config, MAX_PATH))
			{
				_tcscpy( configb, config );
				_tcscat( config, _T("\\bassmidi.sflist") );
				_tcscat( configb, _T("\\bassmidi_b.sflist") );
			}
			LoadFonts(0, config);
			LoadFonts(1, configb);
			if (check_preload())
			{
				BASS_MIDI_StreamLoadSamples( hStream[0] );
				BASS_MIDI_StreamLoadSamples( hStream[1] );
			}
			SetEvent(load_sfevent);
			opend = 1;
			reset_synth[0] = 0;
			reset_synth[1] = 0;
		}
	}

	if (hStOutput)
		BASS_ChannelPlay(hStOutput, FALSE);
	else if (basswasapi)
		BASS_WASAPI_Start();

	while(stop_thread == 0){
		Sleep(100);
	}
	if (hStOutput)
		BASS_ChannelStop(hStOutput);
	else if (basswasapi)
		BASS_WASAPI_Stop(TRUE);
	stop_thread=0;
	if (hStream[ 1 ])
	{
		BASS_StreamFree( hStream[ 1 ] );
		hStream[ 1 ] = 0;
	}
	if (hStream[ 0 ])
	{
		BASS_StreamFree( hStream[ 0 ] );
		hStream[ 0 ] = 0;
	}
	if (hStOutput)
	{
		BASS_StreamFree( hStOutput );
		hStOutput = 0;
	}
	if (basswasapi) {
		BASS_WASAPI_Free();
		FreeLibrary(basswasapi);
		basswasapi = 0;
	}
	if (bassmidi) {
		FreeFonts(1);
		FreeFonts(0);
		FreeLibrary(bassmidi);
		bassmidi =0 ;
	}
	if ( bass ) {
		BASS_Free();
		FreeLibrary(bass);
		bass = 0;
	}
	_endthreadex(0);
	return 0;
}

void DoCallback(int driverNum, int clientNum, DWORD msg, DWORD_PTR param1, DWORD_PTR param2) {
	struct Driver_Client *client = &drivers[driverNum].clients[clientNum];
	DriverCallback(client->callback, client->flags, drivers[driverNum].hdrvr, msg, client->instance, param1, param2);
}

void DoStartClient() {
	if (modm_closed  == 1) {
		DWORD result;
		unsigned int thrdaddr;
		InitializeCriticalSection(&mim_section);
		InitializeCriticalSection(&bass_section);
		processPriority = GetPriorityClass(GetCurrentProcess());
		SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);
		load_sfevent = CreateEvent( 
			NULL,               // default security attributes
			TRUE,               // manual-reset event
			FALSE,              // initial state is nonsignaled
			TEXT("SoundFontEvent")  // object name
			); 
		hCalcThread=(HANDLE)_beginthreadex(NULL,0,threadfunc,0,0,&thrdaddr);
		SetPriorityClass(hCalcThread, REALTIME_PRIORITY_CLASS);
		SetThreadPriority(hCalcThread, THREAD_PRIORITY_TIME_CRITICAL);
		result = WaitForSingleObject(load_sfevent,INFINITE);
		if (result == WAIT_OBJECT_0)
		{
			CloseHandle(load_sfevent);
		}
		modm_closed = 0;
	}
}

void DoStopClient() {
	if (modm_closed == 0){
		stop_thread = 1;
		WaitForSingleObject(hCalcThread, INFINITE);
		CloseHandle(hCalcThread);
		modm_closed = 1;
		SetPriorityClass(GetCurrentProcess(), processPriority);
	}
	DeleteCriticalSection(&bass_section);
	DeleteCriticalSection(&mim_section);
}

void DoResetClient(UINT uDeviceID) {
	/*
	TODO : If the driver's output queue contains any output buffers (see MODM_LONGDATA) whose contents
have not been sent to the kernel-mode driver, the driver should set the MHDR_DONE flag and
clear the MHDR_INQUEUE flag in each buffer's MIDIHDR structure, and then send the client a
MOM_DONE callback message for each buffer.	
	*/
	reset_synth[!!uDeviceID] = 1;
}

LONG DoOpenClient(struct Driver *driver, UINT uDeviceID, LONG* dwUser, MIDIOPENDESC * desc, DWORD flags) {
/*	For the MODM_OPEN message, dwUser is an output parameter.
The driver creates the instance identifier and returns it in the address specified as
the argument. The argument is the instance identifier.
CALLBACK_EVENT Indicates dwCallback member of MIDIOPENDESC is an event handle.
CALLBACK_FUNCTION Indicates dwCallback member of MIDIOPENDESC is the address of a callback function.
CALLBACK_TASK Indicates dwCallback member of MIDIOPENDESC is a task handle.
CALLBACK_WINDOW Indicates dwCallback member of MIDIOPENDESC is a window handle.
*/
	int clientNum;
	if (driver->clientCount == 0) {
		//TODO: Part of this might be done in DoDriverOpen instead.
		DoStartClient();
		DoResetClient(uDeviceID);
		clientNum = 0;
	} else if (driver->clientCount == MAX_CLIENTS) {
		return MMSYSERR_ALLOCATED;
	} else {
		int i;
		for (i = 0; i < MAX_CLIENTS; i++) {
			if (!driver->clients[i].allocated) {
				break;
			}
		}
		if (i == MAX_CLIENTS) {
			return MMSYSERR_ALLOCATED;
		}
		clientNum = i;
	}
	driver->clients[clientNum].allocated = 1;
	driver->clients[clientNum].flags = HIWORD(flags);
	driver->clients[clientNum].callback = desc->dwCallback;
	driver->clients[clientNum].instance = desc->dwInstance;
	*dwUser = clientNum;
	driver->clientCount++;
	SetPriorityClass(GetCurrentProcess(), processPriority);
	//TODO: desc and flags

	DoCallback(uDeviceID, clientNum, MOM_OPEN, 0, 0);
	return MMSYSERR_NOERROR;
}

LONG DoCloseClient(struct Driver *driver, UINT uDeviceID, LONG dwUser) {
/*
If the client has passed data buffers to the user-mode driver by means of MODM_LONGDATA
messages, and if the user-mode driver hasn't finished sending the data to the kernel-mode driver,
the user-mode driver should return MIDIERR_STILLPLAYING in response to MODM_CLOSE.
After the driver closes the device instance it should send a MOM_CLOSE callback message to
the client.
*/

	if (!driver->clients[dwUser].allocated) {
		return MMSYSERR_INVALPARAM;
	}

	driver->clients[dwUser].allocated = 0;
	driver->clientCount--;
	if(driver->clientCount <= 0) {
		DoResetClient(uDeviceID);
		driver->clientCount = 0;
	}
	DoCallback(uDeviceID, dwUser, MOM_CLOSE, 0, 0);
	return MMSYSERR_NOERROR;
}
/* Audio Device Messages for MIDI http://msdn.microsoft.com/en-us/library/ff536194%28v=vs.85%29 */
STDAPI_(DWORD) modMessage(UINT uDeviceID, UINT uMsg, DWORD_PTR dwUser, DWORD_PTR dwParam1, DWORD_PTR dwParam2){
	MIDIHDR *IIMidiHdr;	
	UINT evbpoint;
	struct Driver *driver = &drivers[uDeviceID];
	int exlen = 0;
	unsigned char *sysexbuffer = NULL ;
	DWORD result = 0;
	switch (uMsg) {
	case MODM_OPEN:
		return DoOpenClient(driver, uDeviceID, reinterpret_cast<LONG*>(dwUser), reinterpret_cast<MIDIOPENDESC*>(dwParam1), static_cast<DWORD>(dwParam2));
	case MODM_PREPARE:
		// Reference the MIDIHDR
		IIMidiHdr = (MIDIHDR *)dwParam1;

		if (sizeof(IIMidiHdr->lpData) > LONGMSG_MAXSIZE) return MMSYSERR_INVALPARAM;	// The buffer is too big, invalid parameter
			
		// Lock the MIDIHDR buffer, to prevent the MIDI app from accidentally writing to it
		if (!VirtualLock(IIMidiHdr->lpData, sizeof(IIMidiHdr->lpData))) 
			return MMSYSERR_NOMEM;							// Failed to lock the buffer, the working set is not big enough

		// Mark the buffer as prepared, and say that everything is oki-doki
		IIMidiHdr->dwFlags |= MHDR_PREPARED;
		return MMSYSERR_NOERROR;
	case MODM_UNPREPARE:
		// Reference the MIDIHDR
		IIMidiHdr = (MIDIHDR *)dwParam1;
			
		// Check if the MIDIHDR buffer is valid
		if (!IIMidiHdr) return MMSYSERR_INVALPARAM;					// The buffer doesn't exist, invalid parameter
		if (!(IIMidiHdr->dwFlags & MHDR_PREPARED)) return MMSYSERR_NOERROR;		// Already unprepared, everything is fine
		if (IIMidiHdr->dwFlags & MHDR_INQUEUE) return MIDIERR_STILLPLAYING;		// The buffer is currently being played from the driver, cannot unprepare

		// Mark the buffer as unprepared
		IIMidiHdr->dwFlags &= ~MHDR_PREPARED;

		// Unlock the buffer, and say that everything is oki-doki
		VirtualUnlock(IIMidiHdr->lpData, sizeof(IIMidiHdr->lpData));
		return MMSYSERR_NOERROR;
	case MODM_GETNUMDEVS:
		return 0x2;
	case MODM_GETDEVCAPS:
		return modGetCaps(uDeviceID, reinterpret_cast<MIDIOUTCAPS*>(dwParam1), static_cast<DWORD>(dwParam2));
	case MODM_LONGDATA:
		// Reference the MIDIHDR
		IIMidiHdr = (MIDIHDR *)dwParam1;

		if (!(IIMidiHdr->dwFlags & MHDR_PREPARED)) return MIDIERR_UNPREPARED;

		// Mark the buffer as in queue
		IIMidiHdr->dwFlags &= ~MHDR_DONE;
		IIMidiHdr->dwFlags |= MHDR_INQUEUE;

		// Moving long events to a buffer is not safe,
		// better play them as soon as they get received.
		BASS_MIDI_StreamEvents(hStream[!!uDeviceID], BASS_MIDI_EVENTS_RAW, IIMidiHdr->lpData, sizeof(IIMidiHdr->lpData));	
			
		// Mark the buffer as done
		IIMidiHdr->dwFlags &= ~MHDR_INQUEUE;
		IIMidiHdr->dwFlags |= MHDR_DONE;
			
		// Tell the app that the buffer has been played	
		DoCallback(uDeviceID, static_cast<LONG>(dwUser), MOM_DONE, dwParam1, 0);
		return MMSYSERR_NOERROR;
	case MODM_DATA: {
		EnterCriticalSection(&mim_section);
		evbpoint = evbwpoint;
		if (++evbwpoint >= EVBUFF_SIZE)
			evbwpoint -= EVBUFF_SIZE;
			
		evbuf_t tempevent {
			!!uDeviceID,
			uMsg,
			dwParam1,
			dwParam2
		};
		
		evbuf[evbpoint] = tempevent;
		LeaveCriticalSection(&mim_section);
		
		if (InterlockedIncrement(&evbcount)>=EVBUFF_SIZE) {
			do { /* Nothing */ } while (evbcount >= EVBUFF_SIZE);			
		}
		return MMSYSERR_NOERROR;
	}
	case MODM_GETVOLUME : {
		*(LONG*)dwParam1 = static_cast<LONG>(sound_out_volume_float * 0xFFFF);
		return MMSYSERR_NOERROR;
	}
	case MODM_SETVOLUME: {
		sound_out_volume_float = LOWORD(dwParam1) / (float)0xFFFF;
		sound_out_volume_int = (int)(sound_out_volume_float * (float)0x1000);
		return MMSYSERR_NOERROR;
	}

	case MODM_RESET:
		DoResetClient(uDeviceID);
		return MMSYSERR_NOERROR;
/*
    MODM_GETPOS
	MODM_PAUSE
	//The driver must halt MIDI playback in the current position. The driver must then turn off all notes that are currently on.
    MODM_RESTART
	//The MIDI output device driver must restart MIDI playback at the current position.
   // playback will start on the first MODM_RESTART message that is received regardless of the number of MODM_PAUSE that messages were received.
   //Likewise, MODM_RESTART messages that are received while the driver is already in play mode must be ignored. MMSYSERR_NOERROR must be returned in either case
    MODM_STOP
	//Like reset, without resetting.
	MODM_PROPERTIES
    MODM_STRMDATA
*/
    
	case MODM_CLOSE:
		return DoCloseClient(driver, uDeviceID, static_cast<LONG>(dwUser));
		break;

/*
	MODM_CACHEDRUMPATCHES
    MODM_CACHEPATCHES
*/

	default:
		return MMSYSERR_NOERROR;
		break;
	}
}

