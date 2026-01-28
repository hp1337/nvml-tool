// /home/himesh/nvml-tool/src/main.c
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <nvml.h>
#include <pci/pci.h> // Added for PCI access
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h> // Added for memory mapping
#include <unistd.h>

#define MAX_DEVICES 64
#define MAX_NAME_LEN 256
#define MAX_UUID_LEN 80
#define MAX_SETPOINTS 16

// VRAM Temperature Constants
#define MEM_PATH "/dev/mem"
#define VRAM_REGISTER_OFFSET 0x0000E2A8
#define PG_SZ sysconf(_SC_PAGE_SIZE)

typedef enum {
  CMD_NONE,
  CMD_INFO,
  CMD_POWER,
  CMD_FAN,
  CMD_TEMP,
  CMD_STATUS,
  CMD_LIST,
  CMD_FANCTL,
  CMD_VRAMTEMP  // Add new command here
} command_t;

typedef enum { SUBCMD_NONE, SUBCMD_SET, SUBCMD_RESTORE, SUBCMD_JSON } subcommand_t;

typedef enum { SENSOR_CORE, SENSOR_VRAM } sensor_t; // Added for sensor selection

typedef struct {
  unsigned int temp;
  unsigned int fan;
} setpoint_t;

typedef struct {
  int devices[MAX_DEVICES];
  int device_count;
  int all_devices;
  char uuid[MAX_UUID_LEN];
  int use_uuid;
  command_t command;
  subcommand_t subcommand;
  unsigned int set_value;
  char temp_unit;
  setpoint_t setpoints[MAX_SETPOINTS];
  int setpoint_count;
  sensor_t sensor; // Added sensor preference
} cli_args_t;

// Global variables for signal handling and PCI context
static volatile int running = 1;
static nvmlDevice_t controlled_devices[MAX_DEVICES];
static int controlled_device_ids[MAX_DEVICES];
static int controlled_device_count = 0;
static int is_terminal = 0;

// PCI context for VRAM access
static struct pci_access *pacc = NULL;
static int pci_initialized = 0;

static void cleanup_pci(void) {
  if (pacc) {
    pci_cleanup(pacc);
    pacc = NULL;
    pci_initialized = 0;
  }
}

static int init_pci(void) {
  if (pci_initialized) return 0;

  pacc = pci_alloc();
  if (!pacc) {
    fprintf(stderr, "Error: Failed to allocate PCI structure\n");
    return -1;
  }
  pci_init(pacc);
  pci_scan_bus(pacc);
  pci_initialized = 1;
  return 0;
}

// Helper to find pci_dev matching NVML device
static struct pci_dev* find_pci_dev(nvmlDevice_t device) {
  nvmlPciInfo_t pci_info;
  if (nvmlDeviceGetPciInfo(device, &pci_info) != NVML_SUCCESS) return NULL;

  for (struct pci_dev *dev = pacc->devices; dev; dev = dev->next) {
    pci_fill_info(dev, PCI_FILL_IDENT | PCI_FILL_BASES);

    // Match logic from gputemps
    // Use pciDeviceId (capital D) as per compiler error message
    if ((dev->device_id << 16 | dev->vendor_id) == pci_info.pciDeviceId &&
        (unsigned int)dev->domain == pci_info.domain &&
        dev->bus == (int)pci_info.bus &&
        dev->dev == (int)pci_info.device) {
      return dev;
    }
  }
  return NULL;
}

static int read_vram_temp(nvmlDevice_t device, unsigned int *temp) {
  struct pci_dev *dev = find_pci_dev(device);
  if (!dev) {
    return -1; // Device not found in PCI list
  }

  int fd = open(MEM_PATH, O_RDWR | O_SYNC);
  if (fd < 0) {
    fprintf(stderr, "Error: Failed to open %s (root required): %s\n", MEM_PATH, strerror(errno));
    return -1;
  }

  // Calculate register address
  // dev->base_addr[0] is the BAR0 base address
  uint32_t reg_addr = (dev->base_addr[0] & 0xFFFFFFFF) + VRAM_REGISTER_OFFSET;
  uint32_t base_offset = reg_addr & ~(PG_SZ - 1);
  void *map_base = mmap(0, PG_SZ, PROT_READ, MAP_SHARED, fd, base_offset);
  
  if (map_base == MAP_FAILED) {
    fprintf(stderr, "Error: Failed to map memory: %s\n", strerror(errno));
    close(fd);
    return -1;
  }

  uint32_t reg_value = *((uint32_t *)((char *)map_base + (reg_addr - base_offset)));
  
  // VRAM temp calculation: bits 0-11, divided by 32
  *temp = (reg_value & 0x00000fff) / 0x20;

  munmap(map_base, PG_SZ);
  close(fd);

  return (*temp < 0x7f) ? 0 : -1; // Sanity check from gputemps
}

static void signal_handler(int signum) {
  (void)signum;
  running = 0;
  printf("\nRestoring automatic fan control...\n");

  for (int i = 0; i < controlled_device_count; i++) {
    unsigned int num_fans = 0;
    if (nvmlDeviceGetNumFans(controlled_devices[i], &num_fans) == NVML_SUCCESS) {
      for (unsigned int fan = 0; fan < num_fans; fan++) {
        nvmlDeviceSetFanControlPolicy(controlled_devices[i], fan,
                                      NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW);
      }
    }
  }
}

static int parse_setpoints(int argc, char* argv[], int start_idx, setpoint_t* setpoints,
                           int max_setpoints) {
  int count = 0;

  for (int i = start_idx; i < argc && count < max_setpoints; i++) {
    if (argv[i][0] == '-') break;

    char* colon = strchr(argv[i], ':');
    if (!colon) continue;

    *colon = '\0';
    unsigned int temp = atoi(argv[i]);
    unsigned int fan = atoi(colon + 1);
    *colon = ':';

    if (temp == 0 || fan > 100) {
      fprintf(stderr, "Error: Invalid setpoint '%s' (temp must be >0, fan 0-100%%)\n", argv[i]);
      return -1;
    }

    setpoints[count].temp = temp;
    setpoints[count].fan = fan;
    count++;
  }

  if (count == 0) {
    fprintf(stderr, "Error: No valid setpoints provided\n");
    return -1;
  }

  // Sort setpoints by temperature
  for (int i = 0; i < count - 1; i++) {
    for (int j = i + 1; j < count; j++) {
      if (setpoints[i].temp > setpoints[j].temp) {
        setpoint_t temp_sp = setpoints[i];
        setpoints[i] = setpoints[j];
        setpoints[j] = temp_sp;
      }
    }
  }

  return count;
}

static unsigned int interpolate_fan_speed(unsigned int current_temp, const setpoint_t* setpoints,
                                          int count) {
  if (count == 0) return 0;

  if (current_temp <= setpoints[0].temp) return setpoints[0].fan;
  if (current_temp >= setpoints[count - 1].temp) return setpoints[count - 1].fan;

  for (int i = 0; i < count - 1; i++) {
    if (current_temp >= setpoints[i].temp && current_temp <= setpoints[i + 1].temp) {
      unsigned int temp_range = setpoints[i + 1].temp - setpoints[i].temp;
      unsigned int fan_range = setpoints[i + 1].fan - setpoints[i].fan;
      unsigned int temp_offset = current_temp - setpoints[i].temp;

      return setpoints[i].fan + (fan_range * temp_offset) / temp_range;
    }
  }

  return setpoints[0].fan;
}

static void clear_lines(int count) {
  if (is_terminal && count > 0) {
    for (int i = 0; i < count; i++) printf("\033[1A\033[2K");
  }
}

static void print_usage(const char* name) {
  printf("Usage: %s <command> [subcommand] [options] [args]\n", name);
  printf("\nCommands:\n");
  printf("  info [json]         Show comprehensive device information\n");
  printf("  power [set VALUE]   Show/set power usage and limits\n");
  printf("  fan [set VALUE]     Show/set fan speed (NVML v12+)\n");
  printf("  fan restore         Restore automatic fan control\n");
  printf("  fanctl SETPOINTS    Dynamic fan control with temperature setpoints\n");
  printf("  temp                Show GPU core temperature\n");
  printf("  vramtemp            Show VRAM temperature (requires root)\n"); // Add this
  printf("  status              Show compact status overview\n");
  printf("  list                List all GPUs with index, UUID, and name\n");
  printf("\nDevice Selection:\n");
  printf("  -d, --device LIST   Select devices (default: all)\n");
  printf("  -u, --uuid UUID     Select device by UUID\n");
  printf("\nFan Control Options:\n");
  printf("  -s, --sensor TYPE   Sensor for fan control (default: core)\n");
  printf("                      core - Use GPU Core temperature\n");
  printf("                      vram - Use GDDR6 VRAM temperature (requires root)\n");
  printf("\nOutput Options:\n");
  printf("  --temp-unit UNIT    Temperature unit: C, F, K (default: C)\n");
  printf("  -h, --help          Show this help\n");
  printf("\nExamples:\n");
  printf("  %s fanctl 50:30 70:60 80:90 -d 0  # Core temp control\n", name);
  printf("  %s fanctl 70:30 85:60 -d 0 -s vram  # VRAM temp control\n", name);
}

static double convert_temperature(unsigned int temp_c, char unit) {
  switch (unit) {
  case 'C': return temp_c;
  case 'F': return (temp_c * 9.0 / 5.0) + 32.0;
  case 'K': return temp_c + 273.15;
  default: return temp_c;
  }
}

static int parse_device_range(const char* range_str, int* devices, int max_devices) {
  char* str = strdup(range_str);
  char* token = strtok(str, ",");
  int count = 0;

  while (token && count < max_devices) {
    char* dash = strchr(token, '-');
    if (dash) {
      *dash = '\0';
      int start = atoi(token);
      int end = atoi(dash + 1);
      for (int i = start; i <= end && count < max_devices; i++) devices[count++] = i;
    } else {
      devices[count++] = atoi(token);
    }
    token = strtok(NULL, ",");
  }

  free(str);
  return count;
}

static int find_device_by_uuid(const char* uuid, unsigned int device_count) {
  for (unsigned int i = 0; i < device_count; i++) {
    nvmlDevice_t device;
    char device_uuid[MAX_UUID_LEN];

    if (nvmlDeviceGetHandleByIndex(i, &device) == NVML_SUCCESS &&
        nvmlDeviceGetUUID(device, device_uuid, sizeof(device_uuid)) == NVML_SUCCESS) {
      if (strstr(device_uuid, uuid) != NULL) return i;
    }
  }
  return -1;
}

static void print_device_info_human(nvmlDevice_t device, int device_id, char temp_unit) {
  nvmlReturn_t result;
  char name[MAX_NAME_LEN];
  char uuid[MAX_UUID_LEN];
  unsigned int temperature;
  nvmlMemory_t memory;
  unsigned int fan_speed;
  unsigned int power_usage, power_limit;

  printf("=== Device %d", device_id);

  result = nvmlDeviceGetName(device, name, sizeof(name));
  if (result == NVML_SUCCESS) printf(": %s", name);
  printf(" ===\n");

  result = nvmlDeviceGetUUID(device, uuid, sizeof(uuid));
  if (result == NVML_SUCCESS) printf("UUID:        %s\n", uuid);

  result = nvmlDeviceGetTemperature(device, NVML_TEMPERATURE_GPU, &temperature);
  if (result == NVML_SUCCESS) {
    double temp = convert_temperature(temperature, temp_unit);
    printf("Temperature: %.1f%c\n", temp, temp_unit);
  }

  result = nvmlDeviceGetMemoryInfo(device, &memory);
  if (result == NVML_SUCCESS) {
    double used_pct = (double)memory.used / memory.total * 100.0;
    printf("Memory:      %llu MB / %llu MB (%.1f%%)\n", memory.used / (1024 * 1024),
           memory.total / (1024 * 1024), used_pct);
  }

  result = nvmlDeviceGetFanSpeed(device, &fan_speed);
  if (result == NVML_SUCCESS) printf("Fan Speed:   %u%%\n", fan_speed);

  result = nvmlDeviceGetPowerUsage(device, &power_usage);
  if (result == NVML_SUCCESS) {
    nvmlDeviceGetPowerManagementLimit(device, &power_limit);
    double power_pct = (double)power_usage / power_limit * 100.0;
    printf("Power:       %.2fW / %.2fW (%.1f%%)\n", power_usage / 1000.0, power_limit / 1000.0,
           power_pct);
  }

  printf("\n");
}

static void print_device_info_json(nvmlDevice_t device, int device_id, char temp_unit,
                                   int is_last) {
  char name[MAX_NAME_LEN] = "Unknown";
  char uuid[MAX_UUID_LEN] = "Unknown";
  unsigned int temperature = 0;
  nvmlMemory_t memory = {0};
  unsigned int fan_speed = 0;
  unsigned int power_usage = 0, power_limit = 0;

  nvmlDeviceGetName(device, name, sizeof(name));
  nvmlDeviceGetUUID(device, uuid, sizeof(uuid));
  nvmlDeviceGetTemperature(device, NVML_TEMPERATURE_GPU, &temperature);
  nvmlDeviceGetMemoryInfo(device, &memory);
  nvmlDeviceGetFanSpeed(device, &fan_speed);
  nvmlDeviceGetPowerUsage(device, &power_usage);
  nvmlDeviceGetPowerManagementLimit(device, &power_limit);

  printf("  {\n");
  printf("    \"device_id\": %d,\n", device_id);
  printf("    \"name\": \"%s\",\n", name);
  printf("    \"uuid\": \"%s\",\n", uuid);
  printf("    \"temperature\": %.1f,\n", convert_temperature(temperature, temp_unit));
  printf("    \"temperature_unit\": \"%c\",\n", temp_unit);
  printf("    \"memory_total_mb\": %llu,\n", memory.total / (1024 * 1024));
  printf("    \"memory_used_mb\": %llu,\n", memory.used / (1024 * 1024));
  printf("    \"memory_free_mb\": %llu,\n", memory.free / (1024 * 1024));
  printf("    \"fan_speed_percent\": %u,\n", fan_speed);
  printf("    \"power_usage_watts\": %.2f,\n", power_usage / 1000.0);
  printf("    \"power_limit_watts\": %.2f\n", power_limit / 1000.0);
  printf("  }%s\n", is_last ? "" : ",");
}

static void print_power_cli(nvmlDevice_t device, int device_id) {
  unsigned int power_usage;
  nvmlReturn_t result = nvmlDeviceGetPowerUsage(device, &power_usage);

  if (result == NVML_SUCCESS)
    printf("%d:%.2f\n", device_id, power_usage / 1000.0);
  else
    fprintf(stderr, "%d:Error: %s\n", device_id, nvmlErrorString(result));
}

static void print_fan_cli(nvmlDevice_t device, int device_id) {
  unsigned int fan_speed;
  nvmlReturn_t result = nvmlDeviceGetFanSpeed(device, &fan_speed);

  if (result == NVML_SUCCESS)
    printf("%d:%u\n", device_id, fan_speed);
  else
    fprintf(stderr, "%d:Error: %s\n", device_id, nvmlErrorString(result));
}

static void print_temp_cli(nvmlDevice_t device, int device_id, char temp_unit) {
  unsigned int temperature;
  nvmlReturn_t result = nvmlDeviceGetTemperature(device, NVML_TEMPERATURE_GPU, &temperature);

  if (result == NVML_SUCCESS) {
    double temp = convert_temperature(temperature, temp_unit);
    printf("%d:%.1f\n", device_id, temp);
  } else {
    fprintf(stderr, "%d:Error: %s\n", device_id, nvmlErrorString(result));
  }
}

static void print_vram_temp_cli(nvmlDevice_t device, int device_id) {
  // Ensure PCI is initialized if it hasn't been yet
  if (!pci_initialized) {
    if (init_pci() != 0) {
      fprintf(stderr, "%d:Error: Failed to initialize PCI for VRAM access\n", device_id);
      return;
    }
  }

  unsigned int temp;
  if (read_vram_temp(device, &temp) == 0) {
    printf("%d:%u\n", device_id, temp);
  } else {
    fprintf(stderr, "%d:Error: Failed to read VRAM temp (root required or unsupported GPU)\n", device_id);
  }
}

static void print_status_cli(nvmlDevice_t device, int device_id, char temp_unit) {
  unsigned int temperature = 0, fan_speed = 0, power_usage = 0;

  nvmlDeviceGetTemperature(device, NVML_TEMPERATURE_GPU, &temperature);
  nvmlDeviceGetFanSpeed(device, &fan_speed);
  nvmlDeviceGetPowerUsage(device, &power_usage);

  double temp = convert_temperature(temperature, temp_unit);
  printf("%d:%.1f%c,%u%%,%.1fW\n", device_id, temp, temp_unit, fan_speed, power_usage / 1000.0);
}

static int parse_args(int argc, char* argv[], cli_args_t* args) {
  memset(args, 0, sizeof(cli_args_t));
  args->temp_unit = 'C';
  args->all_devices = 1;
  args->sensor = SENSOR_CORE; // Default to core

  if (argc < 2) return -1;
  static const struct {
    const char* name;
    command_t cmd;
  } commands[] = {{"info", CMD_INFO},     {"power", CMD_POWER}, {"fan", CMD_FAN},
                  {"fanctl", CMD_FANCTL}, {"temp", CMD_TEMP},   {"status", CMD_STATUS},
                  {"list", CMD_LIST},     {"vramtemp", CMD_VRAMTEMP}}; // Add here

  args->command = CMD_NONE;
  for (int i = 0; i < 8; i++) { // Update loop limit from 7 to 8
    if (strcmp(argv[1], commands[i].name) == 0) {
      args->command = commands[i].cmd;
      break;
    }
  }
  if (args->command == CMD_NONE) return -1;

  // Check for subcommand or fanctl setpoints
  int start_idx = 2;
  if (args->command == CMD_FANCTL) {
    args->setpoint_count = parse_setpoints(argc, argv, 2, args->setpoints, MAX_SETPOINTS);
    if (args->setpoint_count < 0) return -1;

    for (int i = 2; i < argc; i++) {
      if (argv[i][0] == '-') {
        start_idx = i;
        break;
      }
      if (i == argc - 1) start_idx = argc;
    }
  } else if (argc > 2 && strcmp(argv[2], "set") == 0) {
    args->subcommand = SUBCMD_SET;
    if (argc > 3) {
      args->set_value = atoi(argv[3]);
      start_idx = 4;
    } else {
      fprintf(stderr, "Error: 'set' requires a value\n");
      return -1;
    }
  } else if (argc > 2 && strcmp(argv[2], "restore") == 0) {
    args->subcommand = SUBCMD_RESTORE;
    start_idx = 3;
  } else if (argc > 2 && strcmp(argv[2], "json") == 0) {
    args->subcommand = SUBCMD_JSON;
    start_idx = 3;
  }

  static struct option long_options[] = {{"device", required_argument, 0, 'd'},
                                         {"uuid", required_argument, 0, 'u'},
                                         {"sensor", required_argument, 0, 's'}, // Added sensor
                                         {"temp-unit", required_argument, 0, 't'},
                                         {"help", no_argument, 0, 'h'},
                                         {0, 0, 0, 0}};

  int opt;
  optind = start_idx;
  while ((opt = getopt_long(argc, argv, "d:u:s:t:h", long_options, NULL)) != -1) {
    switch (opt) {
    case 'd':
      args->device_count = parse_device_range(optarg, args->devices, MAX_DEVICES);
      args->all_devices = 0;
      break;
    case 'u':
      strncpy(args->uuid, optarg, sizeof(args->uuid) - 1);
      args->use_uuid = 1;
      args->all_devices = 0;
      break;
    case 's': // Handle sensor selection
      if (strcmp(optarg, "core") == 0) {
        args->sensor = SENSOR_CORE;
      } else if (strcmp(optarg, "vram") == 0) {
        args->sensor = SENSOR_VRAM;
      } else {
        fprintf(stderr, "Error: Invalid sensor '%s'. Use 'core' or 'vram'.\n", optarg);
        return -1;
      }
      break;
    case 't':
      args->temp_unit = 0;
      if (!strcmp(optarg, "C")) args->temp_unit = 'C';
      if (!strcmp(optarg, "F")) args->temp_unit = 'F';
      if (!strcmp(optarg, "K")) args->temp_unit = 'K';
      if (!args->temp_unit) {
        fprintf(stderr, "Error: Invalid temperature unit '%s'\n", optarg);
        return -1;
      }
      break;
    default: return -1;
    }
  }

  return 0;
}

int main(int argc, char* argv[]) {
  nvmlReturn_t result;
  cli_args_t args;
  unsigned int device_count;

  if (parse_args(argc, argv, &args) != 0) {
    print_usage(argv[0]);
    return 1;
  }

  result = nvmlInit();
  if (result != NVML_SUCCESS) {
    fprintf(stderr, "Error: Failed to initialize NVML (%s)\n", nvmlErrorString(result));
    return 1;
  }

  result = nvmlDeviceGetCount(&device_count);
  if (result != NVML_SUCCESS) {
    fprintf(stderr, "Error: Failed to get device count (%s)\n", nvmlErrorString(result));
    nvmlShutdown();
    return 1;
  }

  if (device_count == 0) {
    fprintf(stderr, "No NVIDIA GPUs found\n");
    nvmlShutdown();
    return 1;
  }

  // Handle UUID selection
  if (args.use_uuid) {
    int device_id = find_device_by_uuid(args.uuid, device_count);
    if (device_id < 0) {
      fprintf(stderr, "Error: Device with UUID '%s' not found\n", args.uuid);
      nvmlShutdown();
      return 1;
    }
    args.devices[0] = device_id;
    args.device_count = 1;
    args.all_devices = 0;
  }

  // Setup device list
  static int all_devs[MAX_DEVICES];
  int* target_devices = args.devices;
  int target_count = args.device_count;

  if (args.all_devices) {
    for (unsigned int i = 0; i < device_count && i < MAX_DEVICES; i++) all_devs[i] = i;
    target_devices = all_devs;
    target_count = device_count;
  }

  // JSON output header
  if (args.subcommand == SUBCMD_JSON && args.command == CMD_INFO) printf("[\n");

  // Execute command for each device
  int error_count = 0;
  for (int i = 0; i < target_count; i++) {
    int device_id = target_devices[i];

    if (device_id >= (int)device_count) {
      fprintf(stderr, "Error: Device ID %d not found (available: 0-%d)\n", device_id,
              device_count - 1);
      error_count++;
      continue;
    }

    nvmlDevice_t device;
    result = nvmlDeviceGetHandleByIndex(device_id, &device);
    if (result != NVML_SUCCESS) {
      fprintf(stderr, "Error: Failed to get device handle for device %d (%s)\n", device_id,
              nvmlErrorString(result));
      error_count++;
      continue;
    }

    switch (args.command) {
    case CMD_INFO:
      if (args.subcommand == SUBCMD_JSON)
        print_device_info_json(device, device_id, args.temp_unit, i == target_count - 1);
      else
        print_device_info_human(device, device_id, args.temp_unit);
      break;

    case CMD_POWER:
      if (args.subcommand == SUBCMD_SET) {
        unsigned int limit_mw = args.set_value * 1000;
        unsigned int min_limit, max_limit;

        result = nvmlDeviceGetPowerManagementLimitConstraints(device, &min_limit, &max_limit);
        if (result != NVML_SUCCESS) {
          fprintf(stderr, "%d:Error: Cannot get power limit constraints (%s)\n", device_id,
                  nvmlErrorString(result));
          error_count++;
          continue;
        }

        if (limit_mw < min_limit || limit_mw > max_limit) {
          fprintf(stderr, "%d:Error: Power limit %uW outside valid range (%.2f-%.2fW)\n", device_id,
                  args.set_value, min_limit / 1000.0, max_limit / 1000.0);
          error_count++;
          continue;
        }

        result = nvmlDeviceSetPowerManagementLimit(device, limit_mw);
        if (result == NVML_SUCCESS) {
          printf("%d:Power limit set to %uW\n", device_id, args.set_value);
        } else {
          fprintf(stderr, "%d:Error: Failed to set power limit (%s)\n", device_id,
                  nvmlErrorString(result));
          error_count++;
        }
      } else {
        print_power_cli(device, device_id);
      }
      break;

    case CMD_FAN:
      if (args.subcommand == SUBCMD_SET || args.subcommand == SUBCMD_RESTORE) {
        unsigned int num_fans = 0;
        result = nvmlDeviceGetNumFans(device, &num_fans);
        if (result != NVML_SUCCESS) {
          fprintf(stderr, "%d:Error: Cannot get number of fans (%s)\n", device_id,
                  nvmlErrorString(result));
          error_count++;
          continue;
        }

        if (num_fans == 0) {
          fprintf(stderr, "%d:Error: Device has no controllable fans\n", device_id);
          error_count++;
          continue;
        }

        if (args.subcommand == SUBCMD_SET && args.set_value > 100) {
          fprintf(stderr, "%d:Error: Fan speed must be between 0-100%%\n", device_id);
          error_count++;
          continue;
        }

        int fan_errors = 0;
        for (unsigned int fan = 0; fan < num_fans; fan++) {
          if (args.subcommand == SUBCMD_SET) {
            result = nvmlDeviceSetFanSpeed_v2(device, fan, args.set_value);
            if (result == NVML_SUCCESS)
              printf("%d:Fan%u:Set to %u%%\n", device_id, fan, args.set_value);
          } else {
            result = nvmlDeviceSetFanControlPolicy(device, fan,
                                                   NVML_FAN_POLICY_TEMPERATURE_CONTINOUS_SW);
            if (result == NVML_SUCCESS)
              printf("%d:Fan%u:Restored to automatic control\n", device_id, fan);
          }

          if (result != NVML_SUCCESS) {
            fprintf(stderr, "%d:Fan%u:Error: %s\n", device_id, fan, nvmlErrorString(result));
            fan_errors++;
          }
        }

        if (fan_errors > 0) {
          error_count++;
        } else if (args.subcommand == SUBCMD_SET) {
          printf("%d:Warning: Fan control is now MANUAL - monitor temperatures!\n", device_id);
          printf("%d:Note: Use 'nvml-tool fan restore -d %d' to restore automatic control\n",
                 device_id, device_id);
        } else {
          printf("%d:All fans restored to automatic temperature-based control\n", device_id);
        }
      } else {
        print_fan_cli(device, device_id);
      }
      break;

    case CMD_TEMP: print_temp_cli(device, device_id, args.temp_unit); break;

    case CMD_VRAMTEMP: print_vram_temp_cli(device, device_id); break;

    case CMD_STATUS: print_status_cli(device, device_id, args.temp_unit); break;

    case CMD_LIST: {
      char uuid[NVML_DEVICE_UUID_BUFFER_SIZE];
      char name[NVML_DEVICE_NAME_BUFFER_SIZE];

      nvmlDeviceGetUUID(device, uuid, sizeof(uuid));
      nvmlDeviceGetName(device, name, sizeof(name));

      printf("%d:%s %s\n", device_id, uuid, name);
    } break;

    case CMD_FANCTL: {
      unsigned int num_fans = 0;
      result = nvmlDeviceGetNumFans(device, &num_fans);
      if (result != NVML_SUCCESS || num_fans == 0) {
        fprintf(stderr, "%d:Error: Device has no controllable fans\n", device_id);
        error_count++;
        continue;
      }

      // Initialize PCI if using VRAM sensor
      if (args.sensor == SENSOR_VRAM) {
        if (init_pci() != 0) {
          fprintf(stderr, "%d:Error: Failed to initialize PCI for VRAM access\n", device_id);
          error_count++;
          continue;
        }
        // Verify we can find the device in PCI list
        if (!find_pci_dev(device)) {
           fprintf(stderr, "%d:Error: Cannot find device in PCI bus for VRAM access\n", device_id);
           error_count++;
           continue;
        }
      }

      if (controlled_device_count < MAX_DEVICES) {
        controlled_devices[controlled_device_count] = device;
        controlled_device_ids[controlled_device_count] = device_id;
        controlled_device_count++;
      }
    } break;

    default: break;
    }
  }

  // JSON output footer
  if (args.subcommand == SUBCMD_JSON && args.command == CMD_INFO) printf("]\n");

  // Handle fanctl main loop
  if (args.command == CMD_FANCTL && controlled_device_count > 0 && error_count == 0) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    atexit(cleanup_pci); // Ensure PCI cleanup on exit

    is_terminal = isatty(STDOUT_FILENO);

    const char* sensor_name = (args.sensor == SENSOR_VRAM) ? "VRAM" : "Core";
    printf("Starting dynamic fan control for %d device(s) using %s temperature (Ctrl-C to exit)\n",
           controlled_device_count, sensor_name);
    printf("Setpoints: ");
    for (int sp = 0; sp < args.setpoint_count; sp++) {
      printf("%u:%u%%", args.setpoints[sp].temp, args.setpoints[sp].fan);
      if (sp < args.setpoint_count - 1) printf(" ");
    }
    printf("\n");

    if (is_terminal) printf("\n");

    // Main control loop
    int first_iteration = 1;
    while (running) {
      if (is_terminal && !first_iteration) {
        clear_lines(controlled_device_count);
      }

      for (int dev_idx = 0; dev_idx < controlled_device_count; dev_idx++) {
        nvmlDevice_t device = controlled_devices[dev_idx];
        int device_id = controlled_device_ids[dev_idx];
        unsigned int current_temp = 0;
        int temp_result = 0;

        if (args.sensor == SENSOR_VRAM) {
          temp_result = read_vram_temp(device, &current_temp);
          if (temp_result != 0) {
            fprintf(stderr, "%d:Error reading VRAM temp. Falling back to Core temp.\n", device_id);
            // Fallback to core if VRAM read fails
            temp_result = nvmlDeviceGetTemperature(device, NVML_TEMPERATURE_GPU, &current_temp);
            if (temp_result != NVML_SUCCESS) {
                fprintf(stderr, "%d:Error reading Core temp. Aborting.\n", device_id);
                running = 0;
                break;
            }
          }
        } else {
          temp_result = nvmlDeviceGetTemperature(device, NVML_TEMPERATURE_GPU, &current_temp);
          if (temp_result != NVML_SUCCESS) {
            fprintf(stderr, "%d:Error: Cannot read temperature (%s)\n", device_id,
                    nvmlErrorString(temp_result));
            running = 0;
            break;
          }
        }

        unsigned int target_fan =
            interpolate_fan_speed(current_temp, args.setpoints, args.setpoint_count);

        unsigned int num_fans = 0;
        nvmlDeviceGetNumFans(device, &num_fans);

        int fan_errors = 0;
        for (unsigned int fan = 0; fan < num_fans; fan++) {
          result = nvmlDeviceSetFanSpeed_v2(device, fan, target_fan);
          if (result != NVML_SUCCESS) {
            fprintf(stderr, "%d:Fan%u:Error: %s\n", device_id, fan, nvmlErrorString(result));
            fan_errors++;
          }
        }

        if (fan_errors == 0) {
          double temp_display = convert_temperature(current_temp, args.temp_unit);
          const char* sensor_label = (args.sensor == SENSOR_VRAM) ? "V" : ""; // Mark VRAM
          printf("%d:%.1f%c%s -> %u%%\n", device_id, temp_display, args.temp_unit, sensor_label, target_fan);
        } else {
          running = 0;
          break;
        }
      }

      fflush(stdout);
      first_iteration = 0;
      if (running) sleep(2);
    }
  }

  cleanup_pci();
  nvmlShutdown();
  return !!error_count;
}

