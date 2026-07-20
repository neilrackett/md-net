import urllib.request
import os
import argparse

MAX_WORDS_PER_LINE = 16  # This results in 32 bytes per line


def read_binary_from_file(file_path):
    with open(file_path, "rb") as file:
        return file.read()


def binary_to_c_array(input_source, output_file, array_name, endian_format="little"):
    offset = 0

    data = read_binary_from_file(input_source)

    # Find the last non-zero byte in the data
    last_non_zero_index = -1
    for i in range(len(data) - 1, -1, -1):
        if data[i] != 0:
            last_non_zero_index = i
            break

    # If the entire file is zero, then effectively no data
    if last_non_zero_index == -1:
        trimmed_data = b''
    else:
        trimmed_data = data[: last_non_zero_index + 1]

    # Ensure the trimmed data is an even number of bytes (we emit 16-bit
    # words). Trimming trailing zeros can strip the padding byte the
    # assembler's `even` directive added, leaving an odd length; restore one
    # zero byte rather than failing the whole build (which would silently
    # leave a stale target_firmware.h).
    if len(trimmed_data) % 2 != 0:
        trimmed_data += b"\x00"

    # Prepare the output content. The array MUST be 4-byte aligned: the
    # RP2040 boot path copies it into ROM_IN_RAM with the XIP stream DMA
    # (COPY_FIRMWARE_TO_RAM_DMA in memfunc.h), and the XIP STREAM_ADDR
    # register silently truncates the source to a 32-bit word boundary.
    # A uint16_t array is only 2-byte aligned, so a 2-mod-4 linker
    # placement makes the stream start 2 bytes early -- the whole
    # cartridge image lands in RAM shifted by 2 bytes and the m68k
    # crashes (bombs / black screen / boot-to-desktop, varying per build
    # because it depends purely on linker layout). aligned(4) makes the
    # truncation a no-op.
    content = f"const uint16_t __attribute__((aligned(4))) {array_name}[] = {{\n"

    # Convert trimmed data to comma-separated hex values with MAX_WORDS_PER_LINE words per line
    for i in range(offset, len(trimmed_data) - offset, 2 * MAX_WORDS_PER_LINE):
        chunk = trimmed_data[i : i + 2 * MAX_WORDS_PER_LINE]

        if endian_format == "big":
            words = [chunk[j] + (chunk[j + 1] << 8) for j in range(0, len(chunk), 2)]
        else:  # little endian
            words = [(chunk[j] << 8) + chunk[j + 1] for j in range(0, len(chunk), 2)]

        content += "    " + ", ".join(f"0x{word:04X}" for word in words) + ",\n"

    # Remove the trailing comma and add closing brace
    content = content.rstrip(",\n") + "\n};\n"
    content += f"uint16_t {array_name}_length = sizeof({array_name}) / sizeof({array_name}[0]);\n\n"

    # Write to output .h file
    with open(output_file, "w") as f:
        f.write(content)

    print(f"{output_file} generated successfully!")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Generate a C array from a binary file, trimming trailing zeros."
    )
    parser.add_argument(
        "--input",
        required=True,
        default="",
        help="Path to the input binary file.",
    )
    parser.add_argument(
        "--output",
        required=True,
        default="",
        help="Output .h (header) file to write the array.",
    )
    parser.add_argument(
        "--array_name",
        required=True,
        default="",
        help="Name of the array to be generated.",
    )
    parser.add_argument(
        "--endian_format",
        required=False,
        default="little",
        help="Endianness of the words in the output array ('little' or 'big').",
    )

    args = parser.parse_args()
    array_name = args.array_name
    output_file = args.output
    input_source = args.input
    endian_format = args.endian_format

    binary_to_c_array(input_source, output_file, array_name, endian_format)
