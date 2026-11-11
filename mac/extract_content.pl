#!/usr/bin/perl
# extract_content.pl -- writes the built-in fun facts / quotes / AFK lines of ui/content.cpp (the Windows app) to mac/content/{facts,quotes,afk}.txt, one line each,
# so the Mac app shows the same text. Run it from the project root after editing the lists:  perl mac/extract_content.pl
use strict; use warnings;
open(my $in,'<:raw','ui/content.cpp') or die "ui/content.cpp: $!"; local $/; my $src=<$in>; close $in;
for my $name (qw(facts quotes afk)){
  $src=~/std::vector<std::wstring>\s+$name\s*=\s*\{(.*?)\};/s or die "list '$name' not found in ui/content.cpp";
  my $block=$1; my @lines;
  while($block=~/L"((?:[^"\\]|\\.)*)"/g){ my $s=$1; $s=~s/\\(["\\])/$1/g; push @lines,$s; }
  die "list '$name' is empty" unless @lines;
  open(my $out,'>:raw',"mac/content/$name.txt") or die "mac/content/$name.txt: $!"; print $out "$_\n" for @lines; close $out;
  print "$name: ".scalar(@lines)." lines\n";
}
