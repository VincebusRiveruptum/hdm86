
/*
   hdmotion - Moves hard disk heads in interesting patterns
   (C) 2005 by Jeremy Stanley

   This program may be distributed freely provided this notice
	 is preserved.

   The author assumes no liability for any damages arising
   out of the use of this program, including but not limited
   to loss of data or desire to open up operational hard drives.

   ---

   80x86 C version by Vincebus Riveruptum

*/


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dos.h>
#include <time.h>
#include <conio.h>
#include <math.h>

typedef int bool;
#define false 0
#define true (!false)

/* Disk geometry */
typedef struct {
  unsigned short size;
  unsigned short info_flags;
  unsigned long  cylinders;
  unsigned long  heads;
  unsigned long  sectors_per_track;
  unsigned long  total_sectors_lo; 
  unsigned long  total_sectors_hi;
  unsigned short bytes_per_sector;
} DRIVE_PARAMS;

typedef struct {
    unsigned short cylinders;          
    unsigned short heads;              
    unsigned short sectors_per_track;
    unsigned long  total_sectors;
    unsigned char  drive_count;        
} CHS_DRIVE_PARAMS;

#ifdef __WATCOMC__
#pragma pack(pop)
#endif

#ifndef M_PI
#define M_PI 3.14159265
#endif

#define BUF_BYTES 8192U   /* sector buffer size (small model friendly) */

unsigned char drvnum = 0;
/*unsigned short bytes_per_sector = 0;*/
unsigned long capacity = 0;
unsigned int buf_sectors = 0;
unsigned int chunk;
size_t bufsize = 0;
unsigned char *buf = NULL;

DRIVE_PARAMS p;
CHS_DRIVE_PARAMS cp;

bool hasLBA = false;
unsigned char version = 0;

/* Prototypes =========================================================*/
void set_chunk(unsigned int chunk);
void print_error(unsigned char code);
bool support_13x(unsigned char drv_num);

bool get_lba_drive_parameters(unsigned char drv_num);
bool get_chs_drive_parameters(unsigned char drv_num);

bool xfer_sectors(unsigned char drv_num, bool write, unsigned long sector, unsigned short count, void far *buf);
bool open_drive(unsigned char drvnum);

void set_chunk(unsigned int chunk);
void move_head(double position);

/* -------------------------
   Extended INT 13H routines
   ------------------------- */

void print_error(unsigned char code){
  const char *message;

  switch(code)
  {
    case 0x01: message = "Invalid function"; break;
    case 0x02: message = "Address mark not found"; break;
    case 0x03: message = "Disk write protected"; break;
    case 0x04: message = "Sector not found"; break;
    case 0x07: message = "Drive parameter activity failed"; break;
    case 0x08: message = "DMA overrun"; break;
    case 0x09: message = "Data boundary error"; break;
    case 0x0A: message = "Bad sector detected"; break;
    case 0x0B: message = "Bad track detected"; break;
    case 0x0C: message = "Unsupported track or invalid media"; break;
    case 0x0E: message = "Control data address mark detected"; break;
    case 0x0F: message = "DMA arbitration level out of range"; break;
    case 0x10: message = "Uncorrectable CRC or ECC error on read"; break;
    case 0x11: message = "Data ECC corrected"; break;
    case 0x31: message = "No media in drive"; break;
    case 0x80: case 0xAA: message = "Drive not ready"; break;
    case 0xB3: message = "Volume in use"; break;
    case 0xCC: message = "Write fault"; break;
		case 0xE0: message = "Status register error"; break;
    case 0xFF: message = "Sense operation failed"; break;
    default: message = "Failure on INT 13";
  }

  fprintf(stderr, "%02x %s\n", code, message);
}

/* Test for INT 13 Extensions support */
bool support_13x(unsigned char drv_num){
  union REGS regs;

  regs.h.ah = 0x41;
  regs.x.bx = 0x55AA;
  regs.h.dl = drv_num;
  
  int86(0x13, &regs, &regs);

  /* No ext. */
  if ((regs.x.cflag & 1) || regs.x.bx != 0xAA55){
    return false;
  }

  version = regs.h.ah;

  printf("\nLBA version: ");
  
  switch(version){
    case 0x01:
      printf("1.x");
      break;
    case 0x20:
      printf("2.0");
      break;
    case 0x21:
      printf("2.1");
      break;
    case 0x30:
      printf("EDD 3.0");
      break;
    default:
      printf("unknown (%02X)", version);
  }
  printf("\n");

  return true;
}


/* Get the size and count of sectors on the drive */
bool get_lba_drive_parameters(unsigned char drv_num){
  union REGS regs;
  struct SREGS sregs;
  /* FP_SEG/FP_OFF need a FAR pointer: in small model &p is near and
     FP_SEG(&p) gives 0, so the BIOS would write to 0000:xxxx */
  void far *fp = (void far *)&p;

  memset(&p, 0, sizeof(p));
  p.size = sizeof(DRIVE_PARAMS);

  segread(&sregs);

  regs.h.ah = 0x48;
  regs.h.dl = drv_num;
  
  sregs.ds = FP_SEG(fp);
  regs.x.si = FP_OFF(fp);

  int86x(0x13, &regs, &regs, &sregs);

  if (regs.x.cflag){
      print_error(regs.h.ah);
      return false;
  }

  if (p.bytes_per_sector == 0)
    p.bytes_per_sector = 512;

  /* only 32 bits of sector count are used */
  capacity = p.total_sectors_hi ? 0xFFFFFFFFUL : p.total_sectors_lo;

  return true;
}

bool get_chs_drive_parameters(unsigned char drv_num){
  union REGS regs;
  struct SREGS sregs;

  segread(&sregs);

  regs.h.ah = 0x08;
  regs.h.dl = drv_num;
  
  sregs.es = 0;
  regs.x.di = 0;

  int86x(0x13, &regs, &regs, &sregs);

  if (regs.x.cflag){
        print_error(regs.h.ah);
        return false;
  }
  /* CHG Drive geometry */
  cp.sectors_per_track = regs.h.cl & 0x3F;
  cp.cylinders = 
    (((unsigned short)(regs.h.cl & 0xC0) << 2) | regs.h.ch) + 1;
  cp.heads     = (unsigned short)regs.h.dh + 1;
  cp.drive_count = regs.h.dl;
  cp.total_sectors = 
      (unsigned long)cp.cylinders * cp.heads * cp.sectors_per_track;

  if (cp.sectors_per_track == 0 || cp.total_sectors == 0)
    return false;

  capacity = cp.total_sectors;
  return true;
}

/* Transfer sectors (LBA via 42h/43h, otherwise CHS via 02h/03h) */
bool xfer_sectors(unsigned char drv_num, bool write, unsigned long sector, unsigned short count, void far *buf){
  union REGS regs;
  struct SREGS sregs;
  unsigned char addx[16];
  unsigned char far *pAddx = (unsigned char far *)addx;
  unsigned int cyl, head, sec;

  segread(&sregs);

  if (hasLBA){
    /* Disk Address Packet, must be pointed to by DS:SI */
    addx[0] = 16;
    addx[1] = 0;
    *(unsigned short *)(addx + 2) = count;
    *(void far **)(addx + 4) = buf;
    *(unsigned long *)(addx + 8) = sector;
    *(unsigned long *)(addx + 12) = 0L;

    regs.h.ah = (write ? 0x43 : 0x42);
    regs.h.al = 0;
    regs.h.dl = drv_num;
    sregs.ds  = FP_SEG(pAddx);
    regs.x.si = FP_OFF(pAddx);
  }else{
    /* LBA -> CHS */
    sec  = (unsigned int)(sector % cp.sectors_per_track) + 1;
    head = (unsigned int)((sector / cp.sectors_per_track) % cp.heads);
    cyl  = (unsigned int)(sector / ((unsigned long)cp.sectors_per_track * cp.heads));

    regs.h.ah = (write ? 0x03 : 0x02);
    regs.h.al = (unsigned char)count;
    regs.h.ch = (unsigned char)(cyl & 0xFF);
    regs.h.cl = (unsigned char)(sec | ((cyl >> 2) & 0xC0));
    regs.h.dh = (unsigned char)head;
    regs.h.dl = drv_num;
    sregs.es  = FP_SEG(buf);
    regs.x.bx = FP_OFF(buf);
  }

  int86x(0x13, &regs, &regs, &sregs);

  if(regs.x.cflag){
    print_error(regs.h.ah);
    return false;
  }

  return true;
}

/* HDMotion functions ====================================== */
bool open_drive(unsigned char drv){
  unsigned int bps = 512;

  hasLBA = support_13x(drv);

  if (!hasLBA)
    fprintf(stderr, "INT 13 Extensions not supported, using legacy 13h disk functions.\n");

  if (hasLBA){
    if (!get_lba_drive_parameters(drv) || capacity == 0){
      fprintf(stderr, "INT 13h fn 48h failed, falling back to CHS.\n");
      hasLBA = false;
    }else{
      bps = p.bytes_per_sector;
    }
  }

  if (!hasLBA && !get_chs_drive_parameters(drv)){
    fprintf(stderr, "Could not read drive geometry.\n");
    return false;
  }

  buf_sectors = BUF_BYTES / bps;
  if (buf_sectors == 0)
    buf_sectors = 1;
  bufsize = (size_t)buf_sectors * bps;

  buf = (unsigned char *)malloc(bufsize);
  if ( !buf ){
    fprintf(stderr, "Failed to allocate sector buffer.\n");
    return false;
  }
  memset(buf, 0, bufsize);

  set_chunk(1);

  if (capacity <= chunk){
    fprintf(stderr, "Drive reports no usable capacity.\n");
    return false;
  }

  printf("Drive %02X: %lu sectors, %u bytes/sector, %s\n",
         drv, capacity, bps, hasLBA ? "LBA" : "CHS");

  return true;
}

void set_chunk(unsigned int n){
  chunk = (n > buf_sectors) ? buf_sectors : n;
}

void move_head(double position){
  static char linebuf[80];
  unsigned long start_sec = 0;
  
  memset(linebuf, ' ', 79);
  linebuf[(int)(position * 79)] = '#';
  linebuf[79] = '\0';
  
  puts(linebuf);

  start_sec = (unsigned long)(position * (capacity - chunk));
  xfer_sectors(drvnum, false, start_sec, chunk, buf);

  if ( kbhit() && (getch() == 27) )
    exit(0);
}


/* main() =======================================================*/

int main(int argc, char **argv){
  double f, s, l, h, amp, x;
  int i, j,heads, repeat;

  if (argc < 2)  {
  	fprintf(stderr, "Please specify a drive number (0x80, 0x81, etc...)\n");
    return 1;
  }

  /* global drvnum: move_head() uses it */
  drvnum = (unsigned char)strtoul(argv[1], NULL, 0);
  
  if ( !open_drive(drvnum) )
  	return 1;

  /* accelerating zigzag */
  s = 0.010;

  for(i = 0; i < 5; ++i){
		for(f = 0.0; f < 1.0; f += s)
	    move_head(f);
	  for(f -= s; f > 0.0; f -= s)
	  	move_head(f);

    s += 0.0075;
  }
  f += s;

  /* tightening zigzag */
  h = 0.90;
  l = 0.10;
  for(; l < h;){
  	for(; f < h; f += s)
    	move_head(f);
    for(; f > l; f -= s)
    	move_head(f);

    h -= 0.05;
    l += 0.05;
  }

  /* widening sinusoid */
  amp = 0.05;

  for(; amp <= 0.50; amp += 0.05){
  	for(x = 0; x < (2 * M_PI); x += M_PI / 32.0){
    	move_head( (sin(x) * amp) + 0.5 );
    }
  }

  /* narrowing sinusoid */
  for(amp = 0.50; amp > 0.0; amp -= 0.05){
  	for(x = 0; x < (2 * M_PI); x += M_PI / 32.0){
    	move_head( (sin(x) * amp) + 0.5 );
    }
  }

  /* widening double-sinusoid */
  amp = 0.05;
  for(; amp <= 0.50; amp += 0.05){
  	for(x = 0; x < (2 * M_PI); x += M_PI / 16.0){
    	f = (sin(x) * amp) + 0.5;
    	move_head( f );
      move_head( 1.0 - f );
    }
  }

  /* narrowing double-sinusoid */
  for(amp = 0.50; amp > 0.0; amp -= 0.05){
  	for(x = 0; x < (2 * M_PI); x += M_PI / 16.0){
    	f = (sin(x) * amp) + 0.5;
    	move_head( f );
      move_head( 1.0 - f );
		}
  }

  /* buncha heads */
	for(heads = 2; heads < 7; ++heads){
    repeat = 160 / heads;
	  for(i = 0; i < repeat; ++i)
	  {
    	for( j = 1; j <= heads; ++j){
		  	move_head((double)j / (heads + 1));
	    }
	  }
  }
  for(; heads > 0; heads -= 2)
	{
    repeat = 160 / heads;
	  for(i = 0; i < repeat; ++i){
    	for(j = 1; j <= heads; ++j){
		  	move_head((double)j / (heads + 1));
	    }
	  }
  }

  /* noise */
  for(i = 0; i < 600; ++i){
  	move_head((double)rand() / RAND_MAX);
  }

  free(buf);

  return 0;
}
