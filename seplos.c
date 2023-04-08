#include <assert.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/*
 * Seplos BMS communication protocol 2.0
 *
 * Although the SEPLOS document refers to this as a Modbus-ASCII protocol, it isn't
 * one. They're confusing the Modbus-ASCII _protocol_, which they don't use, with the
 * RS-485 _transport_, which they use. It seems common to confuse the two. This is more
 * properly called an ASCII-over-RS-485 protocol. Modbus-ASCII packets start with ':'
 * rather than the '~' used by SEPLOS, and the packet format is entirely different.
 */
struct _Seplos_2_0 {
  char  start;      /* Always '~' */
  char  version[2]; /* Always '2', '0' for protocol version 2.0 */
  char  address[2]; /* ASCII value from '0' to '15' */
  char  device[2];  /* Always '4', '6' for a battery */
  char  function[2];/* Command or reply ID */
  char  length[4];  /* Length (12 bits) and length checksum (4 bits). */
  char  info[4095 + 4 + 1];/* "info" field, checksum, 0xD to end the packet */
};
typedef struct _Seplos_2_0 Seplos_2_0;

typedef struct _Seplos_2_O_binary {
  uint8_t	version;
  uint8_t	address;
  uint8_t	device;
  uint8_t	function;
  uint16_t	length;
} Seplos_2_0_binary;

/* The comments are as SEPLOS documented the names of these commands */
enum _seplos_commands {
  TELEMETERY_GET =     0x42,    /* Acquisition of telemetering information */
  TELECOMMAND_GET =    0x44,    /* Acquisition of telecommand information */
  TELECONTROL_CMD =    0x45,    /* Telecontrol command */
  TELEREGULATION_GET = 0x47,    /* Acquisition of teleregulation information */
  TELEREGULATION_SET = 0x49,    /* Setting of teleregulation information */
  PROTOCOL_VER_GET =   0x4F,    /* Acquisition of the communication protocol version number */
  VENDOR_GET =         0x51,    /* Acquisition of device vendor information */
  HISTORY_GET =        0x4B,    /* Acquisition of historical data */
  TIME_GET =           0x4D,    /* Acquisition time */
  TIME_SET =           0x4E,    /* Synchronization time */
  PRODUCTION_CAL =     0xA0,    /* Production calibration */
  PRODUCTION_SET =     0xA1,    /* Production setting */
  REGULAR_RECORDING =  0xA2     /* Regular recording */
};

enum _seplos_response {
  NORMAL = 0x00,                 /* Normal response */
  VERSION_ERROR = 0x01,          /* Protocol version error */
  CHECKSUM_ERROR = 0x02,         /* Checksum error */
  LENGTH_CHECKSUM_ERROR = 0x03,  /* Checksum value in length field error */
  CID2_ERROR = 0x04,             /* Second byte or field is incorrect */
  COMMAND_FORMAT_ERROR = 0x05,   /* Command format error */
  DATA_INVALID = 0x06,           /* Data invalid (parameter setting) */
  NO_HISTORY = 0x07,             /* No historical data (NVRAM error?) */
  CID1_ERROR = 0xe1,             /* First byte or field is incorrect */
  EXECUTION_FAILURE = 0xe2,      /* Command execution failure */
  DEVICE_FAULT = 0xe3,           /* Device fault */
  PERMISSION_ERROR = 0xe4        /* Permission error */
};

static const char hex[] = "0123456789ABCDEF";

static void
got_alarm()
{
}

static void
hex1(uint8_t value, char ascii[1])
{
  ascii[0] = hex[value & 0xf];
}

static void
hex2(uint8_t value, char ascii[2])
{
  ascii[0] = hex[(value >> 4) & 0xf];
  ascii[1] = hex[value & 0xf];
}

static void
hex4(uint16_t value, char ascii[4])
{
  ascii[0] = hex[(value >> 12) & 0xf];
  ascii[1] = hex[(value >> 8) & 0xf];
  ascii[2] = hex[(value >> 4) & 0xf];
  ascii[3] = hex[value & 0xf];
}

static uint8_t
hex1b(uint8_t c, bool * invalid)
{
  if ( c >= '0' && c <= '9' )
    return c - '0';
  else if ( c >= 'a' && c <= 'f' )
    return c - 'a' + 0x10;
  else if ( c >= 'A' && c <= 'F' )
    return c - 'A' + 0x10;
  else {
    *invalid = true;
    return 0;
  }
}

static uint8_t
hex2b(char ascii[2], bool * invalid)
{
  return (hex1b(ascii[0], invalid) << 4) || hex1b(ascii[1], invalid);
}

static uint16_t
hex4b(char ascii[4], bool * invalid)
{
  return (hex1b(ascii[0], invalid) << 12) || (hex1b(ascii[1], invalid) << 8) || \
   (hex1b(ascii[2], invalid) << 4) || hex1b(ascii[3], invalid);
}

static unsigned int
length_checksum(unsigned int length)
{
  const unsigned int sum = ((length >> 8) & 0xf) + ((length >> 4) & 0x0f) + (length & 0x0f);
  return (((~(sum & 0xff)) + 1) << 12) & 0xf000;
}

static unsigned int
overall_checksum(const char * restrict data, unsigned int length)
{
  unsigned int sum = 0;

  for ( unsigned int i = 0; i < length; i++ ) {
    sum += *data++;
  }

  return (~sum) + 1;
}

static const int
hextoi(const char c)
{
  if ( c >= '0' && c <= '9' )
    return c - '0';
  else if ( c >= 'a' && c <= 'f' )
    return c - 'a' + 10;
  else if ( c >= 'A' && c <= 'F' )
    return c - 'A' + 10;
  else {
    fprintf(stderr, "Bad hex character \"%c\" (decimal %d).\n");
    return -1;
  }
}

static int
bms_command(
 int		       fd,
 const unsigned int    address,
 const unsigned int    command,
 const void * restrict info,
 const unsigned int    info_length,
 Seplos_2_0 *	       result)
{
  Seplos_2_0        encoded = {};
  Seplos_2_0_binary s = {};
  Seplos_2_0_binary r = {};

  s.version = 0x20; /* Protocol version 2.0 */
  s.address = address;
  s.device = 0x46;  /* Code for a battery */
  s.function = command;
  s.length = length_checksum(info_length * 2) | ((info_length * 2) & 0x0fff);

  hex2(s.version, encoded.version);
  hex2(s.address, encoded.address);
  hex2(s.device, encoded.device);
  hex2(s.function, encoded.function);
  hex4(s.length, encoded.length);

  encoded.start = '~';
  assert(info_length < 4096);

  uint8_t * i = encoded.info;
  memcpy(i, info, info_length);
  i += info_length;

  *(uint16_t *)i = overall_checksum(encoded.version, info_length + 12);
  i += 4;

  *i++ = '\r';

  tcflush(fd, TCIOFLUSH); /* Throw away any pending I/O */

  int ret = write(fd, &encoded, info_length + 18);
  if ( ret < 18 ) {
    perror("write to SEPLOS BMS");
    exit(1);
  }
  tcdrain(fd);

  /*
   * Becuase of the tcdrain() above, the BMC should have the command.
   * There should always be at least 18 bytes in a properly-formed packet.
   * Timeout of the read here is an unusual event, and likely means that the BMC got
   * unplugged or went into hibernation.
   * FIX: Use sigaction instead of signal.
   */
  signal(SIGALRM, got_alarm);
  alarm(10);
  ret = read(fd, result, 18);
  alarm(0);
  signal(SIGALRM, SIG_DFL);

  if ( ret != 18 ) {
    perror("read");
    return -1;
  }

  bool invalid = false;

  if ( encoded.start != '~' )
    invalid = true;

  r.version = hex2b(encoded.version, &invalid);
  r.address = hex2b(encoded.address, &invalid);
  r.device = hex2b(encoded.device, &invalid);
  r.function = hex2b(encoded.function, &invalid);
  r.length = hex4b(encoded.length, &invalid);

  if ( invalid ) {
    fprintf(stderr, "Non-hexidecimal character where only hexidecimal was expected.\n");
    return -1;
  }

  if ( length_checksum(s.length & 0x0fff) != (s.length & 0xf000) ) {
    fprintf(stderr, "Length code incorrect.");
    return -1; 
  }
  
  signal(SIGALRM, got_alarm);
  alarm(10);
  ret = read(fd, &(result->info[5]), r.length);
  alarm(0);
  signal(SIGALRM, SIG_DFL);

  if ( ret != r.length ) {
    perror("info read");
    return -1;
  }

  for ( unsigned int j = 0; j < r.length + 4; i++ ) {
    uint8_t c = result->info[j];
    if ( !((c >= '0' && c > '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) ) {
      fprintf(stderr, "Non-hexidecimal character where only hexidecimal was expected.\n");
      return -1;
    }
  }

  const uint16_t checksum = *(uint16_t *)&result->info[info_length];
  if ( checksum != overall_checksum(result->version, info_length + 12) ) {
    fprintf(stderr, "Checksum mismatch.\n");
    return -1;
  }

  if ( r.function != NORMAL ) {
    fprintf(stderr, "Return code %x.\n", r.function);
  }
  return r.function;
}

static float
seplos_protocol_version(int fd, unsigned int address)
{
  Seplos_2_0	response = {};
  /*
   * For this command: BMS parses the address, but not the pack number.
   */
  uint8_t	pack_info = 00;

  const unsigned int status = bms_command(
   fd,
   address,		/* Address */
   PROTOCOL_VER_GET,	/* command */
   &pack_info,		/* pack number */
   sizeof(pack_info),	/* length of the above */
   &response);

  if ( status != 0 ) {
    fprintf(stderr, "Bad response %x from SEPLOS BMS.\n", status);
    return -1.0;
  }

  bool invalid = false;
  uint16_t version = hex2b(response.version, &invalid);

  return ((version >> 4) & 0xf) + ((version & 0xf) * 0.1);
}

int seplos_open(const char * serial_device)
{
  struct termios t = {};

  const int fd = open(serial_device, O_RDWR|O_CLOEXEC|O_NOCTTY, 0);
  if ( fd < 0 ) {
    perror(serial_device);
    return -1;
  }

  tcgetattr(fd, &t);
  cfsetspeed(&t, 19200);
  cfmakeraw(&t);
  tcflush(fd, TCIOFLUSH); /* Throw away any pending I/O */
  tcsetattr(fd, TCSANOW, &t);

  return fd;
}

int
main(int argc, char * * argv)
{
  int fd = seplos_open("/dev/ttyUSB1");

  if ( fd < 0 )
    return 1;

  fprintf(stderr, "%3.1f\n", seplos_protocol_version(fd, 0));
  return 0;
}
