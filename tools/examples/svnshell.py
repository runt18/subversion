#!/usr/bin/env python2
#
# svnshell.py : a Python-based shell interface for cruising 'round in
#               the filesystem.
#
######################################################################
#
# Copyright (c) 2000-2001 CollabNet.  All rights reserved.
#
# This software is licensed as described in the file COPYING, which
# you should have received as part of this distribution.  The terms
# are also available at http://subversion.tigris.org/license-1.html.
# If newer versions of this license are posted there, you may use a
# newer version instead, at your option.
#
######################################################################
#

import sys
import string
import time
import re
from random import randint
from svn import fs, util, repos


class SVNShell:
  def __init__(self, pool, path):
    """initialize an SVNShell object"""
    self.pool = pool
    self.taskpool = util.svn_pool_create(pool)
    self.fs_ptr = repos.svn_repos_fs(repos.svn_repos_open(path, pool))
    self.is_rev = 1
    self.rev = fs.youngest_rev(self.fs_ptr, pool)
    self.txn = None
    self.root = fs.revision_root(self.fs_ptr, self.rev, pool)
    self.path = "/"
    self._do_prompt()

  def cmd_help(self, *args):
    """print shell help"""
    print "Available commands:"
    print "  cd DIR       : change the current working directory to DIR"
    print "  exit         : exit the shell"
    print "  ls           : list the contents of the current directory"
    print "  lstxns       : list the transactions available for browsing"
    print "  setrev REV   : set the current revision to browse"
    print "  settxn TXN   : set the current transaction to browse"
    print "  youngest     : list the youngest browsable revision number"
    
  def cmd_cd(self, *args):
    """change directory"""
    args = args[0]
    if len(args) < 1:
      return
    newpath = string.join(filter(None, string.split(args[0], '/')), '/')
    if args[0][0] == '/' or self.path == '/':
      newpath = '/' + newpath
    else:
      newpath = self.path + '/' + newpath

    # cleanup '.' and '..'
    parts = filter(None, string.split(newpath, '/'))
    finalparts = []
    for part in parts:
      if part == '.':
        pass
      elif part == '..':
        if len(finalparts) == 0:
          return
        else:
          finalparts.pop(-1)
      else:
        finalparts.append(part)
    newpath = '/' + string.join(finalparts, '/')
    
    # make sure that path actually exists in the filesystem as a directory
    kind = fs.check_path(self.root, newpath, self.taskpool)
    if kind != util.svn_node_dir:
      print "Path '" + newpath + "' is not a valid filesystem directory."
      return
    self.path = newpath
    util.svn_pool_clear(self.taskpool)

  def cmd_ls(self, *args):
    """list the contents of the current directory"""
    entries = fs.dir_entries(self.root, self.path, self.taskpool)
    keys = entries.keys()
    keys.sort()
    print "   REV   AUTHOR  NODE-REV-ID     SIZE         DATE NAME"
    print "----------------------------------------------------------------------------"
    for entry in keys:
      fullpath = self.path + '/' + entry
      size = ''
      is_dir = fs.is_dir(self.root, fullpath, self.taskpool)
      if is_dir:
        name = entry + '/'
      else:
        size = str(fs.file_length(self.root, fullpath, self.taskpool))
        name = entry
      node_id = fs.unparse_id(fs.dirent_t_id_get(entries[entry]),
                              self.taskpool)
      created_rev = fs.node_created_rev(self.root, fullpath, self.taskpool)
      author = fs.revision_prop(self.fs_ptr, created_rev,
                                util.SVN_PROP_REVISION_AUTHOR, self.taskpool)
      date = fs.revision_prop(self.fs_ptr, created_rev,
                              util.SVN_PROP_REVISION_DATE, self.taskpool)
      date = self._format_date(date, self.taskpool)
     
      print "%6s %8s <%10s> %8s %12s %s" % (created_rev, author,
                                            node_id, size, date, name)
    util.svn_pool_clear(self.taskpool)
  
  def cmd_lstxns(self, *args):
    """list the transactions available for browsing"""
    txns = fs.list_transactions(self.fs_ptr, self.taskpool)
    txns.sort()
    counter = 0
    for txn in txns:
      counter = counter + 1
      print "%8s  " % txn,
      if counter == 6:
        print ""
        counter = 0
    print ""
    util.svn_pool_clear(self.taskpool)
    
  def cmd_setrev(self, *args):
    """set the current revision to view"""
    args = args[0]
    try:
      rev = int(args[0])
      newroot = fs.revision_root(self.fs_ptr, rev, self.pool)
    except:
      print "Error setting the revision to '" + str(rev) + "'"
      return
    fs.close_root(self.root)
    self.root = newroot
    self.rev = rev
    self.is_rev = 1
    self._do_path_landing()
    
  def cmd_settxn(self, *args):
    """set the current transaction to view"""
    args = args[0]
    txn = args[0]
    try:
      txnobj = fs.open_txn(self.fs_ptr, txn, self.pool)
      newroot = fs.txn_root(txnobj, self.pool)
    except:
      print "Error setting the transaction to '" + txn + "'"
      return
    fs.close_root(self.root)
    self.root = newroot
    self.txn = txn
    self.is_rev = 0
    self._do_path_landing()
  
  def cmd_youngest(self, *args):
    """list the youngest revision available for browsing"""
    rev = fs.youngest_rev(self.fs_ptr, self.taskpool)
    print rev
    util.svn_pool_clear(self.taskpool)

  def _format_date(self, date, pool):
    date = util.svn_time_from_nts(date, pool)
    date = time.asctime(time.localtime(date / 1000000))
    return date[4:-8]
  
  def _do_path_landing(self):
    """try to land on self.path as a directory in root, failing up to '/'"""
    not_found = 1
    newpath = self.path
    while not_found:
      kind = fs.check_path(self.root, newpath, self.taskpool)
      if kind == util.svn_node_dir:
        not_found = 0
      else:
        parts = filter(None,string.split(newpath, '/'))
        parts.pop(-1)
        newpath = '/' + string.join(parts, '/')
    self.path = newpath
    util.svn_pool_clear(self.taskpool)

  _errors = ["Huh?",
             "Whatchoo talkin' 'bout, Willis?",
             "Say what?",
             "Nope.  Not gonna do it.",
             "Ehh...I don't think so, chief."]
  def _do_prompt(self):
    """present the prompt and handle the user's input"""
    if self.is_rev:
      prompt = "<rev: " + str(self.rev)
    else:
      prompt = "<txn: " + self.txn
    prompt += " " + self.path + ">$ "
    try:
      input = raw_input(prompt)
    except EOFError:
      return

    ### This will currently screw up when the arguments to the
    ### commands have spaces in them, like 'cd "My Dir"'
    args = filter(None, string.split(input, ' '))
    if len(args) == 0:
      pass
    elif args[0] == 'exit':
      return
    elif not hasattr(self, 'cmd_' + args[0]):
      msg = self._errors[randint(0, len(self._errors) - 1)]
      print msg
    else:
      getattr(self, 'cmd_' + args[0])(args[1:])
    self._do_prompt()
    

def _basename(path):
  "Return the basename for a '/'-separated path."
  idx = string.rfind(path, '/')
  if idx == -1:
    return path
  return path[idx+1:]


def usage(exit):
  if exit:
    output = sys.stderr
  else:
    output = sys.stdout
  output.write(
    "usage: %s REPOS_PATH\n"
    "\n"
    "Once the program has started, type 'help' at the prompt for hints on\n"
    "using the shell.\n" % sys.argv[0])
  sys.exit(exit)

def main():
  if len(sys.argv) < 2:
    usage(1)

  try:
    import termios
    attrs = termios.tcgetattr(sys.stdin)
    termios.tcsetattr(sys.stdin, termios.TCSANOW, attrs)
  except:
    pass
  
  util.run_app(SVNShell, sys.argv[1])

if __name__ == '__main__':
  main()
