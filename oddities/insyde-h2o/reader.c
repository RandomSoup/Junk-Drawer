#include <stdio.h>
#include <stdlib.h>

// Following the structure defined here: https://github.com/SmokelessCPUv2/SmokelessRuntimeEFIPatcher?tab=readme-ov-file#lenovo-bios-unlock.
// This searches for an entry known to exist in FormList, then searches backwards and forwrads to find other entries.

static unsigned char pattern[] = {0x1A, 0xB0, 0xE0, 0xC1, 0x7E, 0x60, 0x75, 0x4B, 0xB8, 0xBB, 0x06, 0x31, 0xEC, 0xFA, 0xAC, 0xF2, 0x00, 0x00, 0x00, 0x00};
static int pattern_size = (int) sizeof (pattern);

void print_pattern(unsigned char *data, int location) {
    for (int i = 0; i < pattern_size; i++) {
        printf("%02x", (int) data[location + i]);
    }
    printf("\n");
}

static unsigned char ending_one[] = {0x00, 0x00, 0x00, 0x00};
static unsigned char ending_two[] = {0x01, 0x00, 0x00, 0x00};
int is_valid(unsigned char *data, int location) {
    int is_valid_one = 1;
    int is_valid_two = 1;
    location += pattern_size - (int) sizeof (ending_one);
    for (int i = 0; i < (int) sizeof (ending_one); i++) {
        if (data[location + i] != ending_one[i]) {
            is_valid_one = 0;
            break;
        }
    }
    for (int i = 0; i < (int) sizeof (ending_two); i++) {
        if (data[location + i] != ending_two[i]) {
            is_valid_two = 0;
            break;
        }
    }
    return is_valid_one || is_valid_two;
}

int main() {
    // Read File
    FILE *f = fopen("File_DXE_driver_H2OFormBrowserDxe_H2OFormBrowserDxe.ffs", "rb");
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *data = malloc(size);
    fread(data, size, 1, f);
    fclose(f);
    
    // Find Pattern
    int location = 0;
    while (1) {
        int found = 1;
        for (int i = 0; i < pattern_size; i++) {
            if (data[location + i] != pattern[i]) {
                found = 0;
                break;
            }
        }
        if (found) {
            break;
        } else {
            location++;
        }
    }
    
    // Backwards
    printf("Backward Search:\n");
    int test = location;
    while (1) {
        test -= pattern_size;
        if (is_valid(data, test)) {
            print_pattern(data, test);
        } else {
            break;
        }
    }
    
    // Current
    printf("Test Pattern:\n");
    print_pattern(data, location);
    
    // Forwards
    printf("Forward Search:\n");
    test = location;
    while (1) {
        test += pattern_size;
        if (is_valid(data, test)) {
            print_pattern(data, test);
        } else {
            break;
        }
    }
}
