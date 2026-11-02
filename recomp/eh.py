"""Read DWARF exception landing pads from an NSO's .eh_frame_hdr and LSDA tables."""


class Reader:
    def __init__(self, image, offset, end=None):
        self.image, self.pos = image, offset
        self.end = len(image) if end is None else end
        if not 0 <= offset <= self.end <= len(image):
            raise ValueError('exception table outside image')

    def integer(self, size, signed=False):
        if self.pos + size > min(self.end, len(self.image)):
            raise ValueError('truncated exception table')
        value = int.from_bytes(self.image[self.pos:self.pos + size], 'little', signed=signed)
        self.pos += size
        return value

    def leb(self, signed=False):
        value = shift = 0
        for _ in range(10):
            byte = self.integer(1)
            value |= (byte & 127) << shift
            shift += 7
            if not byte & 128:
                return value - (1 << shift) if signed and byte & 64 else value
        raise ValueError('oversized LEB128')

    def encoded(self, encoding, data_base=0, indirect=False):
        if encoding == 255:
            raise ValueError('omitted required exception pointer')
        start, form = self.pos, encoding & 15
        if form in (1, 9):
            value = self.leb(form == 9)
        elif form in (0, 2, 3, 4, 10, 11, 12):
            value = self.integer({0: 8, 2: 2, 3: 4, 4: 8, 10: 2, 11: 4, 12: 8}[form], form >= 10)
        else:
            raise ValueError(f'unsupported exception pointer encoding {encoding:#x}')
        application = encoding & 112
        if application == 16:
            value += start
        elif application == 48:
            value += data_base
        elif application:
            raise ValueError(f'unsupported exception pointer base {encoding:#x}')
        if encoding & 128 and not indirect:
            raise ValueError('indirect exception pointer is not supported here')
        return value


def lsda_pads(image, offset, function):
    r = Reader(image, offset)
    encoding = r.integer(1)
    base = function if encoding == 255 else r.encoded(encoding)
    if r.integer(1) != 255:
        r.leb()  # type table is not needed to locate landing pads
    encoding, length = r.integer(1), r.leb()
    r.end = r.pos + length
    if r.end > len(image):
        raise ValueError('call-site table outside image')
    pads = set()
    while r.pos < r.end:
        r.encoded(encoding)
        r.encoded(encoding)
        landing = r.encoded(encoding)
        r.leb()
        if landing:
            pads.add(base + landing)
    return pads


def landing_pads(mod):
    image, hdr = mod.image, mod.nso.eh_start
    if not hdr:
        return set()
    r = Reader(image, hdr)
    if r.integer(1) != 1:
        raise ValueError('unsupported .eh_frame_hdr version')
    ptr, count_enc, table_enc = (r.integer(1) for _ in range(3))
    r.encoded(ptr, hdr)
    count = r.encoded(count_enc, hdr)
    if not 0 <= count <= len(image) // 8:
        raise ValueError('invalid FDE count')
    fdes = []
    for _ in range(count):
        r.encoded(table_enc, hdr)
        fdes.append(r.encoded(table_enc, hdr))
    cies, pads = {}, set()
    for offset in fdes:
        f = Reader(image, offset)
        size = f.integer(4)
        if size in (0, 0xffffffff):
            raise ValueError('unsupported FDE length')
        f.end = offset + 4 + size
        if f.end > len(image):
            raise ValueError('FDE outside image')
        cie_offset = f.pos - f.integer(4)
        if cie_offset not in cies:
            c = Reader(image, cie_offset)
            cie_size = c.integer(4)
            c.end = cie_offset + 4 + cie_size
            if c.integer(4) != 0:
                raise ValueError('invalid CIE identifier')
            version = c.integer(1)
            aug = bytearray()
            while (byte := c.integer(1)):
                aug.append(byte)
            if version == 4:
                if (c.integer(1), c.integer(1)) != (8, 0):
                    raise ValueError('unsupported CIE address size')
            elif version not in (1, 3):
                raise ValueError('unsupported CIE version')
            c.leb(); c.leb(True)
            c.integer(1) if version == 1 else c.leb()
            pc_encoding, lsda_encoding = 0, 255
            if aug:
                if aug[0] != ord('z'):
                    raise ValueError('unsupported CIE augmentation')
                aug_size = c.leb()
                aug_end = c.pos + aug_size
                for kind in aug[1:]:
                    if kind == ord('P'):
                        c.encoded(c.integer(1), indirect=True)
                    elif kind == ord('L'):
                        lsda_encoding = c.integer(1)
                    elif kind == ord('R'):
                        pc_encoding = c.integer(1)
                    elif kind != ord('S'):
                        raise ValueError('unsupported CIE augmentation field')
                if c.pos != aug_end:
                    raise ValueError('CIE augmentation length mismatch')
            cies[cie_offset] = pc_encoding, lsda_encoding, bool(aug)
        pc_encoding, lsda_encoding, augmented = cies[cie_offset]
        function = f.encoded(pc_encoding)
        f.encoded(pc_encoding & 15)
        if augmented:
            aug_size = f.leb()
            aug_end = f.pos + aug_size
            if lsda_encoding != 255:
                lsda = f.encoded(lsda_encoding)
                if lsda:
                    pads.update(lsda_pads(image, lsda, function))
            if f.pos > aug_end or aug_end > f.end:
                raise ValueError('invalid FDE augmentation length')
    result = {mod.base + p for p in pads}
    if any(not mod.in_text(p) for p in result):
        raise ValueError('landing pad outside module text')
    return result
