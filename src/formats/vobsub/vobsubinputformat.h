#ifndef VOBSUBINPUTFORMAT_H
#define VOBSUBINPUTFORMAT_H

/**
 * Copyright (C) 2010-2017 Mladen Milinkovic <max@smoothware.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "../inputformat.h"

#include "mplayer/mp_msg.h"
#include "mplayer/vobsub.h"
#include "mplayer/spudec.h"

#include <QUrl>
#include <QRegExp>

// helper struct for caching and fixing end_pts in some cases
struct sub_text_t {
  sub_text_t(unsigned start_pts, unsigned end_pts, char const *text)
	: start_pts(start_pts), end_pts(end_pts), text(text)
  { }
  unsigned start_pts, end_pts;
  char const *text;
};

/** Converts time stamp in pts format to a string containing the time stamp for the srt format
 *
 * pts (presentation time stamp) is given with a 90kHz resolution (1/90 ms).
 * srt expects a time stamp as  HH:MM:SS:MSS.
 */
std::string pts2srt(unsigned pts) {
  unsigned ms = pts/90;
  unsigned const h = ms / (3600 * 1000);
  ms -= h * 3600 * 1000;
  unsigned const m = ms / (60 * 1000);
  ms -= m * 60 * 1000;
  unsigned const s = ms / 1000;
  ms %= 1000;

  enum { length = sizeof("HH:MM:SS,MSS") };
  char buf[length];
  snprintf(buf, length, "%02d:%02d:%02d,%03d", h, m, s, ms);
  return std::string(buf);
}

/// Dumps the image data to <subtitlename>-<subtitleid>.pgm in Netbpm PGM format
void dump_pgm(std::string const &filename, unsigned counter, unsigned width, unsigned height,
			  unsigned stride, unsigned char const *image, size_t image_size) {

  char buf[500];
  snprintf(buf, sizeof(buf), "%s-%04u.pgm", filename.c_str(), counter);
  FILE *pgm = fopen(buf, "wb");
  if(pgm) {
	fprintf(pgm, "P5\n%u %u %u\n", width, height, 255u);
	for(unsigned i = 0; i < image_size; i += stride) {
	  fwrite(image + i, 1, width, pgm);
	}
	fclose(pgm);
  }
}

namespace SubtitleComposer {
class VobSubInputFormat : public InputFormat
{
	friend class FormatManager;

public:
	virtual bool readBinary(Subtitle &subtitle, const QUrl &url)
	{
#if defined(VERBOSE) || !defined(NDEBUG)
		qputenv("MPLAYER_VERBOSE", QByteArrayLiteral("1"));
#endif

		mp_msg_init();

		const QString filename = url.toLocalFile();
		const QByteArray filebase = filename.left(filename.lastIndexOf('.')).toLatin1();

		// Open the sub/idx subtitles
		void *spu;
		void *vob = vobsub_open(filebase.constData(), 0, 1, 0, &spu);
		if(!vob || !vobsub_get_indexes_count(vob)) {
			qDebug() << "Couldn't open VobSub files '" << filebase << ".idx/.sub'\n";
			return false;
		}

		// list languages
		qDebug() << "Languages:";
		for(size_t i = 0; i < vobsub_get_indexes_count(vob); ++i) {
			char const *const id = vobsub_get_id(vob, i);
			qDebug() << i << ": " << (id ? id : "(no id)");
		}


		int index = 0; // TODO: select subtitle index
		if(index >= 0) {
			if(static_cast<unsigned>(index) >= vobsub_get_indexes_count(vob)) {
				qDebug() << "Index argument out of range: " << index << " ("
				<< vobsub_get_indexes_count(vob) << ")";
				return false;
			}
			vobsub_id = index;
		}

#define min_width 9
#define min_height 1
  // Read subtitles and convert
  void *packet;
  int timestamp; // pts100
  int len;
  unsigned last_start_pts = 0;
  unsigned sub_counter = 1;
//  vector<sub_text_t> conv_subs;
//  conv_subs.reserve(200); // TODO better estimate
  while( (len = vobsub_get_next_packet(vob, &packet, &timestamp)) > 0) {
	if(timestamp >= 0) {
	  spudec_assemble(spu, reinterpret_cast<unsigned char*>(packet), len, timestamp);
	  spudec_heartbeat(spu, timestamp);
	  unsigned char const *image;
	  size_t image_size;
	  unsigned width, height, stride, start_pts, end_pts;
	  spudec_get_data(spu, &image, &image_size, &width, &height, &stride, &start_pts, &end_pts);

	  // skip this packet if it is another packet of a subtitle that
	  // was decoded from multiple mpeg packets.
	  if (start_pts == last_start_pts) {
		continue;
	  }
	  last_start_pts = start_pts;

	  if(width < (unsigned int)min_width || height < (unsigned int)min_height) {
		qWarning() << "WARNING: Image too small " << sub_counter << ", size: " << image_size << " bytes, "
			 << width << "x" << height << " pixels, expected at least " << min_width << "x" << min_height << "\n";
		continue;
	  }

	  if(verbose > 0 && static_cast<unsigned>(timestamp) != start_pts) {
		qWarning() << sub_counter << ": time stamp from .idx (" << timestamp
			 << ") doesn't match time stamp from .sub ("
			 << start_pts << ")\n";
	  }

//	  if(dump_images) {
//		dump_pgm(subname, sub_counter, width, height, stride, image, image_size);
//	  }

	  /*

#ifdef CONFIG_TESSERACT_NAMESPACE
	  char *text = tess_base_api.TesseractRect(image, 1, stride, 0, 0, width, height);
#else
	  char *text = TessBaseAPI::TesseractRect(image, 1, stride, 0, 0, width, height);
#endif
	  if(not text) {
		cerr << "ERROR: OCR failed for " << sub_counter << '\n';
		char const errormsg[] = "VobSub2SRT ERROR: OCR failure!";
		// using raw memory is evil but that's the way Tesseract works
		// If we switch to C++11 we can use unique_ptr.
		text = new char[sizeof(errormsg)];
		memcpy(text, errormsg, sizeof(errormsg));
	  }
	  else {
		  size_t size = strlen(text);
		  while (size > 0 and isspace(text[--size])) {
			  text[size] = '\0';
		  }
	  }
	  if(verb) {
		cout << sub_counter << " Text: " << text << endl;
	  }
	  conv_subs.push_back(sub_text_t(start_pts, end_pts, text));
	  */
	  ++sub_counter;
	}
  }

  /*
  // write the file, fixing end_pts when needed
  for(unsigned i = 0; i < conv_subs.size(); ++i) {
	if(conv_subs[i].end_pts == UINT_MAX && i+1 < conv_subs.size())
	  conv_subs[i].end_pts = conv_subs[i+1].start_pts;

	fprintf(srtout, "%u\n%s --> %s\n%s\n\n", i+1, pts2srt(conv_subs[i].start_pts).c_str(),
			pts2srt(conv_subs[i].end_pts).c_str(), conv_subs[i].text);

	delete[]conv_subs[i].text;
	conv_subs[i].text = 0x0;
  }
*/
//		subtitle.insertLine(new SubtitleLine(stext, showTime, hideTime));

		return true;
	}

protected:
	virtual bool parseSubtitles(Subtitle &, const QString &) const
	{
		return false;
	}

	VobSubInputFormat()
		: InputFormat(QStringLiteral("VobSub"),
		  QStringList(QStringLiteral("idx"))),
		  m_regExp(QStringLiteral("[\\d]+\n([0-2][0-9]):([0-5][0-9]):([0-5][0-9])[,\\.]([0-9]+) --> ([0-2][0-9]):([0-5][0-9]):([0-5][0-9])[,\\.]([0-9]+)\n"))
	{}

	mutable QRegExp m_regExp;
	QUrl m_url;
};
}

#endif
