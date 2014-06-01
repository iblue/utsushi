//  scan-cli.cpp -- command-line interface based scan utility
//  Copyright (C) 2012-2014  SEIKO EPSON CORPORATION
//
//  License: GPL-3.0+
//  Author : AVASYS CORPORATION
//
//  This file is part of the 'Utsushi' package.
//  This package is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License or, at
//  your option, any later version.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//  You ought to have received a copy of the GNU General Public License
//  along with this package.  If not, see <http://www.gnu.org/licenses/>.

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#if HAVE_SIGACTION
#include <signal.h>
#endif

#include <csignal>
#include <cstdlib>

#include <algorithm>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <boost/any.hpp>
#include <boost/bind.hpp>
#include <boost/optional.hpp>
#include <boost/program_options.hpp>
#include <boost/throw_exception.hpp>

#include <utsushi/device.hpp>
#include <utsushi/file.hpp>
#include <utsushi/format.hpp>
#include <utsushi/i18n.hpp>
#include <utsushi/log.hpp>
#include <utsushi/monitor.hpp>
#include <utsushi/option.hpp>
#include <utsushi/pump.hpp>
#include <utsushi/range.hpp>
#include <utsushi/run-time.hpp>
#include <utsushi/scanner.hpp>
#include <utsushi/stream.hpp>
#include <utsushi/value.hpp>

#include "../connexions/hexdump.hpp"
#include "../filters/g3fax.hpp"
#include "../filters/image-skip.hpp"
#include "../filters/jpeg.hpp"
#include "../filters/padding.hpp"
#include "../filters/pdf.hpp"
#include "../filters/pnm.hpp"
#include "../filters/magick.hpp"
#include "../filters/threshold.hpp"
#include "../outputs/tiff.hpp"

namespace po = boost::program_options;

using namespace utsushi;
using namespace _flt_;

using std::invalid_argument;
using std::logic_error;
using std::runtime_error;

static int status = EXIT_SUCCESS;

#define nullptr 0
static pump *pptr (nullptr);

static void
request_cancellation (int sig)
{
  if (pptr) pptr->cancel ();
}

//! Wrap signal registration platform dependencies
/*! \todo Improve error reporting to include signal and handler names
 */
static void
set_signal (int sig, void (*handler) (int))
{
  const string msg_failed
    ("cannot set signal handler (%1%)");
  const string msg_revert
    ("restoring default signal ignore behaviour (%1%)");

#if HAVE_SIGACTION

  struct sigaction sa;
  sa.sa_handler = handler;
  sa.sa_flags = SA_RESTART;
  sigemptyset (&sa.sa_mask);

  struct sigaction rv;

  if (0 != sigaction (sig, &sa, &rv))
    {
      log::error (msg_failed) % sig;
    }
  if (SIG_IGN == rv.sa_handler && SIG_IGN != handler)
    {
      log::brief (msg_revert) % sig;
      sigaction (sig, &rv, 0);
    }

#else

  void (*rv) (int) = std::signal (sig, handler);

  if (SIG_ERR == rv)
    {
      log::error (msg_failed) % sig;
    }
  if (SIG_IGN == rv && SIG_IGN != handler)
    {
      log::brief (msg_revert) % sig;
      std::signal (sig, rv);
    }

#endif  /* HAVE_SIGACTION */
}

//! Turn a \a udi into a scanner supported by a driver
/*! If \a debug functionality is requested, the device I/O connexion
 *  will be wrapped in a \c hexdump logger.
 */
static
scanner::ptr
create (const std::string& udi, bool debug)
{
  monitor mon;
  monitor::const_iterator it;

  if (!udi.empty ())
    {
      it = mon.find (udi);
      if (it != mon.end () && !it->is_driver_set ())
        {
          BOOST_THROW_EXCEPTION
            (runtime_error (_("device found but has no driver")));
        }
    }
  else
    {
      it = std::find_if (mon.begin (), mon.end (),
                         boost::bind (&scanner::info::is_driver_set, _1));
    }

  if (it == mon.end ())
    {
      std::string msg;

      if (!udi.empty ())
        {
          format fmt (_("cannot find '%1%'"));
          msg = (fmt % udi).str ();
        }
      else
        {
          msg = _("no devices available");
        }
      BOOST_THROW_EXCEPTION (runtime_error (msg));
    }

  connexion::ptr cnx (connexion::create (it->connexion (), it->path ()));

  if (debug)
    cnx = make_shared< _cnx_::hexdump > (cnx);

  scanner::ptr rv = scanner::create (cnx, *it);

  if (rv) return rv;

  BOOST_THROW_EXCEPTION
    (runtime_error ((format (_("%1%: not supported")) % udi).str ()));
}

//! Convert a utsushi::option object into a Boost.Program_option
class option_visitor
  : public value::visitor<>
{
public:
  option_visitor (po::options_description& desc, const option& opt,
                  boost::optional< toggle > resampling = boost::none)
    : desc_(desc)
    , opt_(opt)
    , resampling_(resampling)
  {}

  template< typename T >
  void operator() (const T& t) const;

protected:
  po::options_description& desc_;
  const option& opt_;
  boost::optional< toggle > resampling_;
};

template< typename T >
void
option_visitor::operator() (const T& t) const
{
  std::string documentation;
  std::string description;
  std::string key (opt_.key ());

  if (resampling_ && *resampling_)
    {
      if (key.substr (0,10) == "resolution") return;
      if (key.substr (0, 3) == "sw-") key.erase (0, 3);
    }

  if (opt_.text ())             // only translate non-empty strings
    description = _(opt_.text ());

  if (opt_.constraint ())
    {
      if (!description.empty ())
        {
          documentation = (format (_("%1%\n"
                                     "Allowed values: %2%"))
                           % description
                           % *opt_.constraint ()).str ();
        }
      else
        {
          documentation = (format (_("Allowed values: %1%"))
                           % *opt_.constraint ()).str ();
        }
    }
  else
    {
      documentation = description;
    }

  desc_.add_options ()
    (key.c_str (), po::value< T > ()->default_value (t),
     documentation.c_str ());
}

template<>
void
option_visitor::operator() (const toggle& t) const
{
  std::string description;
  std::string key (opt_.key ());

  if (resampling_ && *resampling_)
    {
      if (key.substr (0,10) == "resolution") return;
      if (key.substr (0, 3) == "sw-") key.erase (0, 3);
    }

  if (opt_.text ())             // only translate non-empty strings
    description = _(opt_.text ());

  if (t) key.insert (0, "no-");

  desc_.add_options ()
    (key.c_str (), po::bool_switch (), description.c_str ());
}

template<>
void
option_visitor::operator() (const value::none&) const
{
  std::string description;
  std::string key (opt_.key ());

  if (opt_.text ())             // only translate non-empty strings
    description = _(opt_.text ());

  desc_.add_options ()
    (key.c_str (), description.c_str ());
}

//! Dispatch visitation based upon utsushi::value
/*! When converting an utsushi::option::map, we cannot directly
 *  dispatch a visitor because we iterate over its options.  We need
 *  to pry out the option's value and pass the option as well as the
 *  options_descriptor to the visitor so it can do what it has to when
 *  applied to the value.
 *
 *  This function object does exactly that.
 */
class visit
{
public:
  visit (po::options_description& desc,
         boost::optional< toggle > resampling = boost::none)
    : desc_(desc)
    , resampling_(resampling)
  {}

  void operator() (const option& opt) const
  {
    if (opt.is_read_only ()) return;

    value val (opt);
    option_visitor v (desc_, opt, resampling_);

    val.apply (v);
  }

protected:
  po::options_description& desc_;
  boost::optional< toggle > resampling_;
};

class run
{
public:
  run (option::map::ptr acts)
    : acts_(acts)
  {}

  void
  operator() (const po::variables_map::value_type& kv)
  {
    if ("dont-scan" == kv.first) return;

    result_code rc = (*acts_)[kv.first].run ();
    if (rc)
      {
        std::cerr << rc.message () << "\n";
        status = EXIT_FAILURE;
      }
  }

  option::map::ptr acts_;
};

//! Collect command-line arguments so they can be assigned all at once
/*! Setting option values one at a time may be fraught with constraint
 *  violations.  For that reason, it is safer to try setting all the
 *  values in one fell swoop.  This function object collects all the
 *  options it sees during its life-time in a value::map and tries to
 *  assign that when it goes out of scope.
 */
class assign
{
public:
  assign (option::map::ptr opts)
    : opts_(opts)
  {}

  ~assign ()
  {
    if (opts_->count ("enable-resampling"))
      {
        toggle t = value ((*opts_)["enable-resampling"]);
        if (vm_.count ("enable-resampling"))
          t = vm_["enable-resampling"];

        if (t)
          {
            replace ("resolution");
            replace ("resolution-x");
            replace ("resolution-y");
            replace ("resolution-bind");
          }
      }

    opts_->assign (vm_);
  }

  void
  operator() (const po::variables_map::value_type& kv)
  {
    if (kv.second.defaulted ()) return;

    /**/ if (boost::any_cast< quantity > (&kv.second.value ()))
      {
        vm_[kv.first] = kv.second.as< quantity > ();
      }
    else if (boost::any_cast< string > (&kv.second.value ()))
      {
        vm_[kv.first] = kv.second.as< string > ();
      }
    else if (boost::any_cast< bool > (&kv.second.value ()))
      {
        std::string key (kv.first);

        if ("no-" != key.substr (0, 3))
          {
            vm_[key] = toggle (kv.second.as< bool > ());
          }
        else
          {
            key.erase (0, 3);
            vm_[key] = toggle (!kv.second.as< bool > ());
          }
      }
    else
      {
        format fmt (_("option parser internal inconsistency\n"
                      "  key = %1%"));
        BOOST_THROW_EXCEPTION (logic_error ((fmt % kv.first).str ()));
      }
  }

protected:
  void replace (const std::string& k)
  {
    value::map::iterator it (vm_.find (k));

    if (vm_.end () == it) return;

    vm_["sw-" + k] = it->second;
    vm_.erase (it);
  }

  option::map::ptr opts_;
  value::map vm_;
};

//! Reset all options after the first one that was not recognized
/*! Boost.Program_options is a bit of an eager beaver when you
 *  allow_unregistered() options.  This helper lets you reset those
 *  options that were prematurely recognized so that later passes will
 *  see them again.
 *
 *  \todo Find a more elegant kludge
 *  \note This code was copied from run-time.cpp
 */
struct unrecognize
{
  bool found_first_;

  unrecognize ()
    : found_first_(false)
  {}

  po::option
  operator() (po::option& item)
  {
    found_first_ |= item.string_key.empty ();
    found_first_ |= item.unregistered;
    item.unregistered = found_first_;

    return item;
  }
};

void
on_notify (log::priority level, std::string message)
{
  std::cerr << message << '\n';

  if (log::ERROR >= level) status = EXIT_FAILURE;
}

int
main (int argc, char *argv[])
{
  try
    {
      run_time rt (argc, argv, i18n);

      if (rt.count ("version"))
        {
          std::cout << rt.version ();
          return EXIT_SUCCESS;
        }

      // Positional arguments disguised as (undocumented) options
      //
      // Note that both positional arguments are optional.  This may
      // introduce a minor ambiguity if the first is not given on the
      // command-line.  The first positional argument is supposed to
      // be the device and hence should correspond to a valid UDI.  If
      // this is not the case and only a single positional argument is
      // specified, we will assume it is the output destination.

      std::string udi;
      std::string uri;

      po::options_description cmd_pos_opts;
      cmd_pos_opts
        .add_options ()
        ("UDI", (po::value< std::string > (&udi)),
         _("image acquisition device to use"))
        ("URI", (po::value< std::string > (&uri)),
         _("output destination to use"))
        ;

      po::positional_options_description cmd_pos_args;
      cmd_pos_args
        .add ("UDI", 1)
        .add ("URI", 1)
        ;

      // Self-documenting command options

      std::string fmt;

      po::variables_map cmd_vm;
      po::options_description cmd_opts (_("Utility options"));
      cmd_opts
        .add_options ()
        ("debug", _("log device I/O in hexdump format"))
        ("image-format", (po::value< std::string > (&fmt)),
         _("output image format\n"
           "PNM, PNG, JPEG, PDF, TIFF "
           "or one of the device supported transfer-formats.  "
           "The explicitly mentioned types are normally inferred from"
           " the output file name."))
        ;

      po::options_description cmd_line;
      cmd_line
        .add (cmd_opts)
        .add (cmd_pos_opts)
        ;

      po::parsed_options cmd (po::command_line_parser (rt.arguments ())
                              .options (cmd_line)
                              .positional (cmd_pos_args)
                              .allow_unregistered ()
                              .run ());

      std::transform (cmd.options.begin (), cmd.options.end (),
                      cmd.options.begin (),
                      unrecognize ());

      po::store (cmd, cmd_vm);
      po::notify (cmd_vm);

      if (uri.empty () && !scanner::info::is_valid (udi))
        {
          uri = udi;
          udi.clear ();
        }

      if (rt.count ("help"))
        {
          std::cout << "\n"
                    << cmd_opts
                    << "\n";

          monitor mon;

          if (udi.empty () && mon.empty ())
            return EXIT_SUCCESS;
        }

      scanner::ptr device (create (udi, cmd_vm.count ("debug")));

      // Self-documenting device and add-on options

      po::variables_map act_vm;
      po::options_description dev_acts (_("Device actions"));
      std::for_each (device->actions ()->begin (),
                     device->actions ()->end (),
                     visit (dev_acts));

      if (!device->actions ()->empty ())
        {
          dev_acts
            .add_options ()
            ("dont-scan", po::bool_switch (),
             _("Only perform the actions given on the command-line."
               "  Do not perform image acquisition."))
            ;
        }

      po::variables_map dev_vm;
      po::options_description dev_opts (_("Device options"));
      boost::optional< toggle > resampling;
      if (device->options ()->count ("enable-resampling"))
        {
          toggle t = value ((*device->options ())["enable-resampling"]);
          resampling = t;
        }
      std::for_each (device->options ()->begin (),
                     device->options ()->end (),
                     visit (dev_opts, resampling));

      po::variables_map add_vm;
      po::options_description add_opts (_("Add-on options"));
      filter::ptr blank_skip = make_shared< _flt_::image_skip > ();
      std::for_each (blank_skip->options ()->begin (),
                     blank_skip->options ()->end (),
                     visit (add_opts));

      if (rt.count ("help"))
        {
          if (!device->actions ()->empty ())
            std::cout << dev_acts;

          std::cout << dev_opts
                    << add_opts
                    << "\n"
                    <<
            // FIXME: use word-wrapping instead of hard-coded newlines
            (_("Note: device options may be ignored if their prerequisites"
               " are not satisfied.\nA '--duplex' option may be ignored if"
               " you do not select the ADF, for example.\n"));

          return EXIT_SUCCESS;
        }

      run_time::sequence_type dev_argv;
      dev_argv = po::collect_unrecognized (cmd.options,
                                           po::exclude_positional);

      po::parsed_options act (po::command_line_parser (dev_argv)
                              .options (dev_acts)
                              .allow_unregistered ()
                              .run ());

      dev_argv = po::collect_unrecognized (act.options,
                                           po::include_positional);

      po::parsed_options dev (po::command_line_parser (dev_argv)
                              .options (dev_opts)
                              .allow_unregistered ()
                              .run ());

      dev_argv = po::collect_unrecognized (dev.options,
                                           po::include_positional);

      po::parsed_options add (po::command_line_parser (dev_argv)
                              .options (add_opts)
                              .run ());

      dev_argv = po::collect_unrecognized (add.options,
                                           po::include_positional);

      if (uri.empty () && !dev_argv.empty ())
        {
          uri = dev_argv[0];
        }

      po::store (act, act_vm);
      po::notify (act_vm);

      std::for_each (act_vm.begin (), act_vm.end (),
                     run (device->actions ()));

      po::store (dev, dev_vm);
      po::notify (dev_vm);

      std::for_each (dev_vm.begin (), dev_vm.end (),
                     assign (device->options ()));

      po::store (add, add_vm);
      po::notify (add_vm);

      std::for_each (add_vm.begin (), add_vm.end (),
                     assign (blank_skip->options ()));

      if (act_vm.count ("dont-scan")
          && !act_vm["dont-scan"].defaulted ())
        return status;

      // Infer desired image format from file extension

      if (!uri.empty ())
        {
          fs::path path (uri);

          /**/ if (".pnm"  == path.extension ()) fmt = "PNM";
          else if (".png"  == path.extension ()) fmt = "PNG";
          else if (".jpg"  == path.extension ()) fmt = "JPEG";
          else if (".jpeg" == path.extension ()) fmt = "JPEG";
          else if (".pdf"  == path.extension ()) fmt = "PDF";
          else if (".tif"  == path.extension ()) fmt = "TIFF";
          else if (".tiff" == path.extension ()) fmt = "TIFF";
          else
            log::alert
              ("cannot infer image format from destination: '%1%'") % path;
        }

      // Configure the filter chain

      option::map& om (*device->options ());
      stream::ptr str = make_shared< stream > ();

      const std::string xfer_raw = "image/x-raster";
      const std::string xfer_jpg = "image/jpeg";
      std::string xfer_fmt = device->get_context ().content_type ();

      bool bilevel = (om["image-type"] == "Gray (1 bit)");
      filter::ptr threshold (make_shared< threshold > ());
      try
        {
          (*threshold->options ())["threshold"]
            = value (om["threshold"]);
        }
      catch (const std::out_of_range&)
        {
          log::error ("Falling back to default threshold value");
        }

      filter::ptr jpeg_compress (make_shared< jpeg::compressor > ());
      try
        {
          (*jpeg_compress->options ())["quality"]
            = value ((om)["jpeg-quality"]);
        }
      catch (const std::out_of_range&)
        {
          log::error ("Falling back to default JPEG compression quality");
        }

      toggle force_extent = true;
      quantity width  = -1.0;
      quantity height = -1.0;
      try
        {
          force_extent = value (om["force-extent"]);
          width   = value (om["br-x"]);
          width  -= value (om["tl-x"]);
          height  = value (om["br-y"]);
          height -= value (om["tl-y"]);
        }
      catch (const std::out_of_range&)
        {
          force_extent = false;
          width  = -1.0;
          height = -1.0;
        }
      if (force_extent) force_extent = (width > 0 || height > 0);

      toggle resample = false;
      if (om.count ("enable-resampling"))
        resample = value (om["enable-resampling"]);

      filter::ptr magick;
      if (resample)
        {
          magick = make_shared< _flt_::magick > ();

          toggle bound = true;
          quantity res_x  = -1.0;
          quantity res_y  = -1.0;

          if (om.count ("sw-resolution-x"))
            {
              res_x = value (om["sw-resolution-x"]);
              res_y = value (om["sw-resolution-y"]);
            }
          if (om.count ("sw-resolution-bind"))
            bound = value (om["sw-resolution-bind"]);

          if (bound)
            {
              res_x = value (om["sw-resolution"]);
              res_y = value (om["sw-resolution"]);
            }

          (*magick->options ())["resolution-x"] = res_x;
          (*magick->options ())["resolution-y"] = res_y;
          (*magick->options ())["force-extent"] = force_extent;
          (*magick->options ())["width"]  = width;
          (*magick->options ())["height"] = height;
        }

      if (fmt == "PNG")
        {
          if (!magick) magick = make_shared< _flt_::magick > ();

          (*magick->options ())["bilevel"] = toggle (bilevel);

          quantity thr = value (om["threshold"]);
          thr *= 100.0;
          thr /= (dynamic_pointer_cast< range >
                  (om["threshold"].constraint ()))->upper ();
          (*magick->options ())["threshold"] = thr;

          if (!resample)
            {
              toggle bound = true;
              quantity res_x  = -1.0;
              quantity res_y  = -1.0;

              if (om.count ("resolution-x"))
                {
                  res_x = value (om["resolution-x"]);
                  res_y = value (om["resolution-y"]);
                }
              if (om.count ("resolution-bind"))
                bound = value (om["resolution-bind"]);

              if (bound)
                {
                  res_x = value (om["resolution"]);
                  res_y = value (om["resolution"]);
                }

              (*magick->options ())["resolution-x"] = res_x;
              (*magick->options ())["resolution-y"] = res_y;
              (*magick->options ())["force-extent"] = force_extent;
              (*magick->options ())["width"]  = width;
              (*magick->options ())["height"] = height;
            }
          (*magick->options ())["image-format"] = fmt;
        }

      toggle skip_blank = !bilevel; // \todo fix filter limitation
      quantity skip_thresh = -1.0;
      try
        {
          skip_thresh = value ((*blank_skip->options ())["blank-threshold"]);
        }
      catch (const std::out_of_range&)
        {
          skip_blank = false;
          log::error ("Disabling blank skip functionality");
        }
      // Don't even try skipping of completely white images.  We are
      // extremely unlikely to encounter any of those.
      skip_blank = (skip_blank
                    && (quantity (0.) < skip_thresh));

      /**/ if (xfer_raw == xfer_fmt) {}
      else if (xfer_jpg == xfer_fmt) {}
      else
        {
          log::alert
            ("unsupported transfer format: '%1%'") % xfer_fmt;
        }

      /**/ if ("PNM" == fmt)
        {
          /**/ if (xfer_raw == xfer_fmt)
            str->push (make_shared< padding > ());
          else if (xfer_jpg == xfer_fmt)
            str->push (make_shared< jpeg::decompressor > ());
          else
            BOOST_THROW_EXCEPTION
              (runtime_error
               ((format (_("conversion from %1% to %2% is not supported"))
                 % xfer_fmt
                 % fmt)
                .str ()));
          if (skip_blank) str->push (blank_skip);
          if (magick)
            str->push (magick);
          else if (force_extent)
            str->push (make_shared< bottom_padder > (width, height));
          if (xfer_jpg == xfer_fmt && bilevel)
            str->push (threshold);
          str->push (make_shared< pnm > ());
        }
      else if (fmt == "PNG")
        {
          /**/ if (xfer_raw == xfer_fmt)
            {
              str->push (make_shared< padding > ());
              str->push (make_shared< pnm > ());
            }
          else if (xfer_jpg == xfer_fmt)
            str->push (make_shared< jpeg::decompressor > ());
          else
            BOOST_THROW_EXCEPTION
              (runtime_error
               ((format (_("conversion from %1% to %2% is not supported"))
                 % xfer_fmt
                 % fmt)
                .str ()));
          if (skip_blank) str->push (blank_skip);
          if (magick)
            str->push (magick);
        }
      else if ("JPEG" == fmt)
        {
          if (bilevel)
            BOOST_THROW_EXCEPTION
              (logic_error
               (_("JPEG does not support bi-level imagery")));

          /**/ if (xfer_raw == xfer_fmt)
            {
              str->push (make_shared< padding > ());
              if (skip_blank) str->push (blank_skip);
              if (magick)
                str->push (magick);
              else if (force_extent)
                str->push (make_shared< bottom_padder > (width, height));
              str->push (jpeg_compress);
            }
          else if (xfer_jpg == xfer_fmt)
            {
              if (magick || force_extent || skip_blank)
                {
                  str->push (make_shared< jpeg::decompressor > ());
                  if (skip_blank)
                    str->push (blank_skip);
                  if (magick)
                    str->push (magick);
                  else if (force_extent)
                    str->push (make_shared< bottom_padder >
                                (width, height));
                  str->push (jpeg_compress);
                }
            }
          else
            {
              BOOST_THROW_EXCEPTION
                (runtime_error
                 ((format (_("conversion from %1% to %2% is not supported"))
                   % xfer_fmt
                   % fmt)
                  .str ()));
            }
        }
      else if ("PDF" == fmt)
        {
          /**/ if (xfer_raw == xfer_fmt)
            {
              str->push (make_shared< padding > ());
              if (skip_blank) str->push (blank_skip);
              if (magick)
                str->push (magick);
              else if (force_extent)
                str->push (make_shared< bottom_padder > (width, height));

              if (bilevel)
                {
                  str->push (make_shared< g3fax > ());
                }
              else
                {
                  str->push (jpeg_compress);
                }
            }
          else if (xfer_jpg == xfer_fmt)
            {
              if (magick || force_extent || skip_blank || bilevel)
                {
                  str->push (make_shared< jpeg::decompressor > ());
                  if (skip_blank) str->push (blank_skip);
                  if (magick)
                    str->push (magick);
                  else if (force_extent)
                    str->push (make_shared< bottom_padder > (width, height));

                  if (bilevel)
                    {
                      str->push (threshold);
                      str->push (make_shared< g3fax > ());
                    }
                  else
                    {
                      str->push (jpeg_compress);
                    }
                }
            }
          else
            {
              BOOST_THROW_EXCEPTION
                (runtime_error
                 ((format (_("conversion from %1% to %2% is not supported"))
                   % xfer_fmt
                   % fmt)
                  .str ()));
            }
          str->push (make_shared< pdf > ());
        }
      else if ("TIFF" == fmt)
        {
          /**/ if (xfer_raw == xfer_fmt)
            str->push (make_shared< padding > ());
          else if (xfer_jpg == xfer_fmt)
            str->push (make_shared< jpeg::decompressor > ());
          else
            BOOST_THROW_EXCEPTION
              (runtime_error
               ((format (_("conversion from %1% to %2% is not supported"))
                 % xfer_fmt
                 % fmt)
                .str ()));
          if (skip_blank) str->push (blank_skip);
          if (magick)
            str->push (magick);
          else if (force_extent)
            str->push (make_shared< bottom_padder > (width, height));
          if (xfer_jpg == xfer_fmt && bilevel)
            str->push (threshold);
        }
      else
        {
          log::brief
            ("unsupported image format requested, passing data as is");
        }

      // Create an output device

      if (device->is_single_image ()
          || uri.empty ()
          || "PDF"  == fmt
          || "TIFF" == fmt)
        {
          if (uri.empty ()) uri = "/dev/stdout";
          if ("TIFF" == fmt)
            {
              str->push (make_shared< _out_::tiff_odevice > (uri));
            }
          else
            {
              str->push (make_shared< file_odevice > (uri));
            }
        }
      else
        {
          fs::path path (uri);
          path_generator gen (!path.parent_path ().empty ()
                              ? path.parent_path () / path.stem ()
                              : path.stem (), path.extension ().native ());

          str->push (make_shared< file_odevice > (gen));
        }

      pump p (device);
      pptr = &p;                // for use in request_cancellation

      set_signal (SIGTERM, request_cancellation);
      set_signal (SIGINT , request_cancellation);

      p.connect (on_notify);
      p.start (str);
    }
  catch (std::exception& e)
    {
      std::cerr << e.what () << "\n";
      return EXIT_FAILURE;
    }
  catch (...)
    {
      return EXIT_FAILURE;
    }

  exit (status);
}
