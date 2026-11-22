// The text of the AWS managed policies, models/iam-managed-policies.jsonl, in the binary of what imports this file:
// main.bob, and the one test that reads it all, as Bober cannot embed a file. ELF only (Linux): Mach-O names the
// section and the symbols otherwise. The path is the directory's that bober runs in, not this file's: the repo's root.
// A build that cannot find it leaves a broken object in build/, which then fails as "not an x86-64 or arm64 ELF64
// relocatable object" until build/ is removed.

#include <stdint.h>

__asm__(".section .rodata\n.global catalog_data\ncatalog_data:\n.incbin \"models/iam-managed-policies.jsonl\"\n"
        ".global catalog_end\ncatalog_end:\n.previous\n");
extern const unsigned char catalog_data[], catalog_end[];

/// The text's address and length, into out.
void catalog_text(uint64_t *out) { out[0] = (uintptr_t)catalog_data, out[1] = catalog_end - catalog_data; }
