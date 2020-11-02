//
// ssd1306.cpp
//
// mt32-pi - A bare-metal Roland MT-32 emulator for Raspberry Pi
// Copyright (C) 2020  Dale Whinham <daleyo@gmail.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include <type_traits>

#include "lcd/font6x8.h"
#include "lcd/ssd1306.h"
#include "utility.h"

// Compile-time (constexpr) font conversion functions.
// The SSD1306 stores pixel data in columns, but our source font data is stored as rows.
// These templated functions generate column-wise versions of our font at compile-time.
namespace
{
	using CharData = u8[8];

	// Iterate through each row of the character data and collect bits for the nth column
	static constexpr u8 SingleColumn(const CharData& CharData, u8 nColumn)
	{
		u8 bit = 5 - nColumn;
		u8 column = 0;

		for (u8 i = 0; i < 8; ++i)
			column |= (CharData[i] >> bit & 1) << i;

		return column;
	}

	// Double the height of the character by duplicating column bits into a 16-bit value
	static constexpr u16 DoubleColumn(const CharData& CharData, u8 nColumn)
	{
		u8 singleColumn = SingleColumn(CharData, nColumn);
		u16 column = 0;

		for (u8 i = 0; i < 8; ++i)
		{
			bool bit = singleColumn >> i & 1;
			column |= bit << i * 2 | bit << (i * 2 + 1);
		}

		return column;
	}

	// Templated array-like structure with precomputed font data
	template<size_t N, class F>
	class Font
	{
	public:
		// Result type of conversion function determines array type
		using Column = typename std::result_of<F& (const CharData&, u8)>::type;
		using ColumnData = Column[6];

		constexpr Font(const CharData(&CharData)[N], F Function) : mCharData{ 0 }
		{
			for (size_t i = 0; i < N; ++i)
				for (u8 j = 0; j < 6; ++j)
					mCharData[i][j] = Function(CharData[i], j);
		}

		const ColumnData& operator[](size_t nIndex) const { return mCharData[nIndex]; }

	private:
		ColumnData mCharData[N];
	};
}

// Single and double-height versions of the font
constexpr auto FontSingle = Font<Utility::ArraySize(Font6x8), decltype(SingleColumn)>(Font6x8, SingleColumn);
constexpr auto FontDouble = Font<Utility::ArraySize(Font6x8), decltype(DoubleColumn)>(Font6x8, DoubleColumn);

// Drawing constants
constexpr u8 Width      = 128;
constexpr u8 BarWidth   = 12;
constexpr u8 BarSpacing = 2;
constexpr u8 BarOffset  = 2;

CSSD1306::CSSD1306(CI2CMaster *pI2CMaster, u8 nAddress, u8 nHeight, TLCDRotation Rotation)
	: CMT32LCD(),
	  m_pI2CMaster(pI2CMaster),
	  m_nAddress(nAddress),
	  m_nHeight(nHeight),
	  m_Rotation(Rotation),

	  m_Framebuffer{0x40}
{
	assert(nHeight == 32 || nHeight == 64);
}

bool CSSD1306::Initialize()
{
	assert(m_pI2CMaster != nullptr);

	if (!(m_nHeight == 32 || m_nHeight == 64))
		return false;

	const u8 pageAddrRange  = m_nHeight == 32 ? 0x03 : 0x07;
	const u8 segRemap       = m_Rotation == TLCDRotation::Inverted ? 0xA0 : 0xA1;
	const u8 comScanDir     = m_Rotation == TLCDRotation::Inverted ? 0xC0 : 0xC8;
	const u8 multiplexRatio = static_cast<u8>(m_nHeight - 1);
	const u8 comPins        = m_nHeight == 32 ? 0x02 : 0x12;

	const u8 initSequence[] =
	{
		0xAE,				// Screen off
		0x81,				// Set contrast
			0x7F,			// 00-FF, default to half

		0xA6,				// Normal display

		0x20,				// Set memory addressing mode
			0x0,			// 00 = horizontal
		0x21,				// Set column start and end address
			0x00,
			0x7F,
		0x22,				// Set page address range
			0x00,
			pageAddrRange,

		segRemap,			// Set segment remap
		0xA8,				// Set multiplex ratio
			multiplexRatio,	// Screen height - 1

		comScanDir,			// Set COM output scan direction
		0xD3,				// Set display offset
			0x00,			// None
		0xDA,				// Set com pins hardware configuration
			comPins,		// Alternate COM config and disable COM left/right

		0xD5,				// Set display oscillator
			0x80,			// Default value
		0xD9,				// Set precharge period
			0x22,			// Default value
		0xDB,				// Set VCOMH deselected level
			0x20,			// Default value

		0x8D,				// Set charge pump
		0x14,				// VCC generated by internal DC/DC circuit

		0xA4,				// Resume to RAM content display
		0xAF				// Set display on
	};

	u8 buffer[] = {0x80, 0};
	for (auto byte : initSequence)
	{
		buffer[1] = byte;
		m_pI2CMaster->Write(m_nAddress, buffer, 2);
	}

	return true;
}

void CSSD1306::WriteFramebuffer() const
{
	// Copy entire framebuffer
	m_pI2CMaster->Write(m_nAddress, m_Framebuffer, m_nHeight * 16 + 1);
}

void CSSD1306::SetPixel(u8 nX, u8 nY)
{
	// Ensure range is within 0-127 for x, 0-63 for y
	nX &= 0x7F;
	nY &= 0x3F;

	// The framebuffer starts with the 0x40 byte so that we can write out the entire
	// buffer to the I2C device in one shot, so offset by 1
	m_Framebuffer[((nY & 0xF8) << 4) + nX + 1] |= 1 << (nY & 7);
}

void CSSD1306::ClearPixel(u8 nX, u8 nY)
{
	// Ensure range is within 0-127 for x, 0-63 for y
	nX &= 0x7F;
	nY &= 0x3F;

	m_Framebuffer[((nY & 0xF8) << 4) + nX + 1] &= ~(1 << (nY & 7));
}

void CSSD1306::DrawChar(char chChar, u8 nCursorX, u8 nCursorY, bool bInverted, bool bDoubleWidth)
{
	size_t rowOffset = nCursorY * Width * 2;
	size_t columnOffset = nCursorX * (bDoubleWidth ? 12 : 6) + 5;

	// FIXME: Won't be needed when the full font is implemented in font6x8.h
	if (chChar == '\xFF')
		chChar = '\x80';

	for (u8 i = 0; i < 6; ++i)
	{
		u16 fontColumn = FontDouble[static_cast<u8>(chChar - ' ')][i];

		// Don't invert the leftmost column or last two rows
		if (i > 0 && bInverted)
			fontColumn ^= 0x3FFF;

		// Shift down by 2 pixels
		fontColumn <<= 2;

		// Upper half of font
		size_t offset = rowOffset + columnOffset + (bDoubleWidth ? i * 2 : i);

		m_Framebuffer[offset] = fontColumn & 0xFF;
		m_Framebuffer[offset + Width] = (fontColumn >> 8) & 0xFF;
		if (bDoubleWidth)
		{
			m_Framebuffer[offset + 1] = m_Framebuffer[offset];
			m_Framebuffer[offset + Width + 1] = m_Framebuffer[offset + Width];
		}
	}
}

void CSSD1306::DrawPartLevels(u8 nFirstRow, bool bDrawPeaks)
{
	const size_t firstPageOffset = nFirstRow * Width;
	const u8 totalPages          = m_nHeight / 8 - 2;
	const u8 barHeight           = m_nHeight - 8 * 2;

	// For each part
	for (u8 i = 0; i < 9; ++i)
	{
		u8 pageValues[totalPages];
		const u8 partLevelPixels = m_PartLevels[i] * barHeight;
		const u8 peakLevelPixels = m_PeakLevels[i] * barHeight;
		const u8 fullPages       = partLevelPixels / 8;
		const u8 remainder       = partLevelPixels % 8;

		for (u8 j = 0; j < fullPages; ++j)
			pageValues[j] = 0xFF;

		for (u8 j = fullPages; j < totalPages; ++j)
			pageValues[j] = 0;

		if (remainder)
			pageValues[fullPages] = 0xFF << (8 - remainder);

		// Peak meters
		if (bDrawPeaks && peakLevelPixels)
		{
			const u8 peakPage  = peakLevelPixels / 8;
			const u8 remainder = peakLevelPixels % 8;

			if (remainder)
				pageValues[peakPage] |= 1 << 8 - remainder;
			else
				pageValues[peakPage - 1] |= 1;
		}

		// For each bar column
		for (u8 j = 0; j < BarWidth; ++j)
		{
			// For each bar row
			for (u8 k = 0; k < totalPages; ++k)
			{
				size_t offset = firstPageOffset;

				// Start BarOffset pixels from the left
				offset += BarOffset;

				// Start from bottom-most page
				offset += (totalPages - 1) * Width - k * Width;

				// i'th bar + j'th bar column
				offset += i * (BarWidth + BarSpacing) + j;

				// +1 to skip 0x40 byte
				m_Framebuffer[offset + 1] = pageValues[k];
			}
		}
	}
}

void CSSD1306::Print(const char* pText, u8 nCursorX, u8 nCursorY, bool bClearLine, bool bImmediate)
{
	while (*pText && nCursorX < 20)
	{
		DrawChar(*pText++, nCursorX, nCursorY);
		++nCursorX;
	}

	if (bClearLine)
	{
		while (nCursorX < 20)
			DrawChar(' ', nCursorX++, nCursorY);
	}

	if (bImmediate)
		WriteFramebuffer();
}

void CSSD1306::Clear(bool bImmediate)
{
	memset(m_Framebuffer + 1, 0, Width * m_nHeight / 8);
	if (bImmediate)
		WriteFramebuffer();
}

void CSSD1306::Update(const CMT32Synth& Synth)
{
	CMT32LCD::Update(Synth);

	UpdatePartLevels(Synth);
	UpdatePeakLevels();

	DrawPartLevels(0);
	u8 statusRow = m_nHeight == 32 ? 1 : 3;
	Print(m_TextBuffer, 0, statusRow, true);
	WriteFramebuffer();
}
