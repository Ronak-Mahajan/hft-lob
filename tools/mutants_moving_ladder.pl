#!/usr/bin/perl
# mutants_moving_ladder.pl - does each moving-ladder test catch a bug?
#
# Copies include/ and src/ into a scratch directory once per mutant, plants
# one hand-made bug in include/lob/book.hpp, builds lob_bench and lob_replay
# from the copy, and runs each moving-ladder test on its own:
#
#   8a 8b 8c 8d   lob_bench --quick --only 8x   (the four unit scenarios)
#   9             lob_bench --quick --only 9    (drifting-price fuzz vs ref::Market)
#   selftest      lob_replay --selftest         (part 2: causal ladders, differential on)
#
# A test KILLS a mutant when it exits non-zero: a failed check, or the book's
# own LOB_ASSERT stopping the process. The first row builds the unmodified
# sources, where every test must pass.
#
# Usage (from the repository root):  perl tools/mutants_moving_ladder.pl [scratch-dir]
# Environment: CXX (default g++).
use strict;
use warnings;
use File::Path qw(make_path remove_tree);
use File::Copy qw(copy);
use File::Find;

my $cxx = $ENV{CXX} // 'g++';
my $scratch = shift // 'mutants.tmp';
my $flags = '-std=c++20 -O2 -Wall -Wextra -pthread';
my $exe = $^O =~ /msys|MSWin32|cygwin/ ? '.exe' : '';

# id, what the bug does, text in include/lob/book.hpp, replacement.
my @mutants = (
    ['none', 'unmodified sources', '', ''],
    ['M1', 'levels that stay on the ladder keep their old level_idx after a shift',
     "set_bit(ni);\n                    relink(L, ni, st);", "set_bit(ni);"],
    ['M2', 'levels that leave the ladder keep a ladder level_idx',
     "relink(L, kOverflowLevel, st);", ";"],
    ['M3', 'levels pulled onto the ladder are not marked in the bitmap',
     "            set_bit(li);\n            relink(levels_[li], li, st);", "            relink(levels_[li], li, st);"],
    ['M4', 'a shift visits the levels in the wrong direction',
     "bids_.rewindow(old_price, shifted, k > 0, moved_);\n            asks_.rewindow(old_price, shifted, k > 0, moved_);",
     "bids_.rewindow(old_price, shifted, k < 0, moved_);\n            asks_.rewindow(old_price, shifted, k < 0, moved_);"],
    ['M5', 'the pull onto the ladder stops one tick short of its top',
     "nb + (w - 1) * g", "nb + (w - 2) * g"],
    ['M6', 'the cached best is not rebuilt after levels come onto the ladder',
     "            it = overflow_.erase(it);\n        }\n        recompute_best();", "            it = overflow_.erase(it);\n        }"],
    ['M7', 'a slot a level left is not cleared',
     "                levels_[li] = PriceLevel{};\n", ""],
    ['M8', 'adds deep in the book count as near-touch misses',
     "        if (behind >= uint64_t{width_} / 2 * g) return false;   // deep in the book\n", ""],
    ['M9', 'adds off the would-be grid count as near-touch misses',
     "        if (price % g != 0) return false;             // not on the grid a moved ladder would use\n", ""],
    ['M10', 'a move never changes the grid',
     "        const Price g = grid_for(center);\n        const uint64_t w = width_;",
     "        const Price g = band_ ? tick_ : grid_for(center);\n        const uint64_t w = width_;"],
    ['M11', 'a move does not reset the near-touch miss count',
     "        near_misses_ = 0;\n", ""],
);
my @tests = (
    ['8a', "lob_bench$exe --quick --only 8a"],
    ['8b', "lob_bench$exe --quick --only 8b"],
    ['8c', "lob_bench$exe --quick --only 8c"],
    ['8d', "lob_bench$exe --quick --only 8d"],
    ['9', "lob_bench$exe --quick --only 9"],
    ['selftest', "lob_replay$exe --selftest"],
);

sub slurp { my $f = shift; open my $h, '<:raw', $f or die "$f: $!"; local $/; my $s = <$h>; close $h; $s =~ s/\r\n/\n/g; $s }
sub spew { my ($f, $s) = @_; open my $h, '>:raw', $f or die "$f: $!"; print $h $s; close $h }

sub copy_tree {
    my ($dst) = @_;
    remove_tree($dst) if -d $dst;
    for my $top ('include', 'src') {
        find({ no_chdir => 1, wanted => sub {
            my $rel = $File::Find::name;
            if (-d $rel) { make_path("$dst/$rel"); return; }
            spew("$dst/$rel", slurp($rel));
        } }, $top);
    }
}

my (%killed_by, %kills);
my $baseline_failed = 0;
printf "%-5s %-72s", 'id', 'bug planted in include/lob/book.hpp';
printf " %-9s", $_->[0] for @tests;
print "\n";
for my $m (@mutants) {
    my ($id, $what, $from, $to) = @$m;
    my $dir = "$scratch/$id";
    copy_tree($dir);
    if (length $from) {
        my $f = "$dir/include/lob/book.hpp";
        my $s = slurp($f);
        my $n = () = $s =~ /\Q$from\E/g;
        die "$id: expected exactly one match, found $n\n" unless $n == 1;
        $s =~ s/\Q$from\E/$to/;
        spew($f, $s);
    }
    for my $t (['lob_bench', 'src/main.cpp'], ['lob_replay', 'src/replay_main.cpp']) {
        my $cmd = "cd $dir && $cxx $flags -I include $t->[1] -o $t->[0]$exe 2> build_$t->[0].log";
        system($cmd) == 0 or die "$id: build failed: $cmd\n";
    }
    printf "%-5s %-72s", $id, $what;
    for my $t (@tests) {
        my $rc = system("cd $dir && ./$t->[1] > test_$t->[0].log 2>&1");
        my $code = $rc == -1 ? 255 : $rc >> 8;
        my $mark = $code == 0 ? 'pass' : "KILLED($code)";
        if ($id eq "none") { $mark = $code == 0 ? "pass" : "FAIL($code)"; $baseline_failed = 1 if $code; }
        elsif ($code != 0) { $killed_by{$id}++; $kills{$t->[0]}++; }
        printf " %-9s", $mark;
    }
    print "\n";
    remove_tree($dir);
}
print "\n";
my $ok = 1;
for my $m (@mutants[1 .. $#mutants]) {
    next if $killed_by{$m->[0]};
    print "mutant $m->[0] survived every test\n";
    $ok = 0;
}
for my $t (@tests) {
    printf "test %-9s kills %d of %d mutants\n", $t->[0], $kills{$t->[0]} // 0, scalar(@mutants) - 1;
    $ok = 0 unless $kills{$t->[0]};
}
$ok = 0 if $baseline_failed;
print $ok ? "RESULT: every mutant killed, every test kills at least one\n" : "RESULT: FAIL\n";
remove_tree($scratch);
exit($ok ? 0 : 1);
