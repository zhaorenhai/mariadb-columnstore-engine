/* Copyright (C) 2019 MariaDB Corporaton

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
   MA 02110-1301, USA. */


#ifndef CONFIG_H_
#define CONFIG_H_

#include <boost/property_tree/ptree.hpp>
#include <boost/thread.hpp>
#include <sys/types.h>

#include <string>

/* TODO.  Need a config change listener impl. */

namespace storagemanager
{
class Config : public boost::noncopyable
{
    public:
        static Config *get();
        virtual ~Config();
        
        std::string getValue(const std::string &section, const std::string &key) const;
        
    private:
        Config();
        
        void reload();
        void reloadThreadFcn();
        struct ::timespec last_mtime;
        mutable boost::mutex mutex;
        boost::thread reloader;
        boost::posix_time::time_duration reloadInterval;
        
        std::string filename;
        boost::property_tree::ptree contents;
        bool die;
};

}

#endif
