#!/usr/bin/perl
# tools/perf_json.pl - builds results/perf_20190130.json from the committed
# run logs only, and refuses to if any run failed its checks, any run's book
# digests differ from the differential run's, or the runs came from more than
# one source commit.
# usage: perl tools/perf_json.pl results > results/perf_20190130.json
#        perl tools/perf_json.pl results 20191230 > results/perf_20191230.json
use strict;
use warnings;

my $dir = shift or die "usage: $0 RESULTS_DIR\n";
sub slurp { my $f = shift; open my $h, '<', $f or die "$f: $!"; local $/; my $s = <$h>; $s =~ s/\r//g; $s }
sub n { my $s = shift; $s =~ s/,//g; $s + 0 }
sub jstr { my $s = shift; $s =~ s/\\/\\\\/g; $s =~ s/"/\\"/g; "\"$s\"" }

# JSON writer; obj() keeps keys in the order given.
sub js {
    my ($v, $ind) = @_;
    $ind //= "";
    my $in = "$ind  ";
    if (ref $v eq "OBJ") {
        return "{\n" . join(",\n", map { $in . jstr($_->[0]) . ": " . js($_->[1], $in) } @$v) . "\n$ind}";
    }
    if (ref $v eq "ARRAY") {
        return "[]" unless @$v;
        return "[" . join(", ", map { js($_, $in) } @$v) . "]" unless grep { ref } @$v;
        return "[\n" . join(",\n", map { $in . js($_, $in) } @$v) . "\n$ind]";
    }
    return "null" unless defined $v;
    return $v =~ /^-?(?:0|[1-9]\d*)(?:\.\d+)?$/ ? $v : jstr($v);
}
sub obj { my @kv = @_; my @o; while (@kv) { my $k = shift @kv; push @o, [$k, shift @kv]; } bless \@o, "OBJ" }

sub common {
    my ($s, $file) = @_;
    my %c;
    ($c{cpu}) = $s =~ /^  cpu: (.+?) \| logical processors: (\d+)/m or die "$file: cpu";
    ($c{logical}) = $s =~ /logical processors: (\d+)/;
    ($c{os}) = $s =~ /^  os: (.+)$/m;
    ($c{power}) = $s =~ /^  power: (.+)$/m;
    ($c{compiler}, $c{flags}) = $s =~ /^  compiler: (.+?) \| flags: (.+)$/m;
    ($c{commit}) = $s =~ /^  source: git commit (\S+)/m;
    ($c{run}) = $s =~ /^run: (.+?) local \| command: (.+)$/m;
    ($c{command}) = $s =~ /\| command: (.+)$/m;
    ($c{tsc_cpuid}) = $s =~ /CPUID 15h nominal TSC frequency: ([\d.]+) GHz/;
    ($c{tsc_windows}, $c{tsc_median}) = $s =~ /five 1 s windows: ([\d. ]+?) GHz \| median ([\d.]+) GHz/;
    ($c{tsc_pass}) = $s =~ /TSC over the whole pass \([\d.]+ s\): ([\d.]+) GHz/;
    ($c{invariant}) = $s =~ /invariant TSC \(CPUID 80000007h EDX bit 8\): (\w+)/;
    ($c{digest}) = $s =~ /book digest chain over \d+ chunk boundaries: ([0-9a-f]+)/;
    ($c{chunks}) = $s =~ /book digest chain over (\d+) chunk boundaries/;
    $c{pass} = $s =~ /^RESULT: PASS$/m ? 1 : 0;
    ($c{messages}) = $s =~ /^  messages ([\d,]+) of /m;
    $c{messages} = n($c{messages}) if defined $c{messages};
    ($c{dropped}) = $s =~ /dropped_out_of_band ([\d,]+)/;
    ($c{bad_length}) = $s =~ /\| bad_length ([\d,]+) \| dropped/;
    ($c{unknown}) = $s =~ /unknown order id ([\d,]+)/;
    my @cs = $s =~ /^  chunk +\d+ \|.*book digest ([0-9a-f]+)$/mg;
    $c{chunk_digests} = \@cs;
    return \%c;
}

sub median { my @v = sort { $a <=> $b } @_; @v % 2 ? $v[$#v/2] : ($v[@v/2-1] + $v[@v/2]) / 2 }
sub r { sprintf "%.${\ ($_[1] // 2)}f", $_[0] }

my (@thr, $ecore, @lat, $demux, $presplit, $replay);
my %seen_digest;
my $ref_chunks;
my @all_common;

for my $i (1 .. 5) {
    my $f = "perf_20190130_run_throughput_$i.log";
    my $s = slurp("$dir/$f");
    my $c = common($s, $f);
    my ($secs, $msgs, $mps, $ns) = $s =~ /timed dispatch ([\d.]+) s for ([\d,]+) messages \| ([\d.]+) M msgs\/s \| ([\d.]+) ns\/msg/ or die "$f: result";
    my ($cyc) = $s =~ /([\d.]+) cycles\/msg/;
    my ($cpu) = $s =~ /^result \(single core, cpu (\d+)\)/m;
    push @thr, { file => $f, c => $c, secs => $secs, mps => $mps, ns => $ns, cyc => $cyc, cpu => $cpu };
    push @all_common, [$f, $c];
}
{
    my $f = "perf_20190130_run_throughput_ecore.log";
    my $s = slurp("$dir/$f");
    my $c = common($s, $f);
    my ($secs, $msgs, $mps, $ns) = $s =~ /timed dispatch ([\d.]+) s for ([\d,]+) messages \| ([\d.]+) M msgs\/s \| ([\d.]+) ns\/msg/ or die "$f";
    my ($cpu) = $s =~ /^result \(single core, cpu (\d+)\)/m;
    $ecore = { file => $f, c => $c, secs => $secs, mps => $mps, ns => $ns, cpu => $cpu };
    push @all_common, [$f, $c];
}
my @types = ('A', 'F', 'E', 'C', 'X', 'D', 'U', 'other', 'orders', 'all');
for my $i (1 .. 3) {
    my $f = "perf_20190130_run_latency_$i.log";
    my $s = slurp("$dir/$f");
    my $c = common($s, $f);
    my ($omin, $op50, $op99) = $s =~ /empty timer pair, 100,000 samples: min (\d+) \| p50 (\d+) \| p99 (\d+) cycles/ or die "$f ovh";
    my ($ghz) = $s =~ /ns = cycles \/ ([\d.]+)/;
    my ($zero) = $s =~ /samples clamped to 0 after overhead subtraction: ([\d,]+)/;
    my ($cyc_tab) = $s =~ /\n  TSC cycles\n(.*?)\n\n/s or die "$f cycles";
    my ($ns_tab) = $s =~ /\n  nanoseconds\n(.*?)\n\n/s or die "$f ns";
    my (%cy, %nsx);
    for my $pair ([\$cyc_tab, \%cy], [\$ns_tab, \%nsx]) {
        for my $line (split /\n/, ${$pair->[0]}) {
            next unless $line =~ /^  (\S+)\s+([\d,]+) \|\s+([\d.]+) \|\s+(\d+) \|\s+(\d+) \|\s+(\d+) \|\s+(\d+) \|\s+(\d+) \|\s+(\d+) \|\s+(\d+)$/;
            $pair->[1]{$1} = { n => n($2), mean => $3, min => $4, p50 => $5, p90 => $6, p99 => $7, p999 => $8, p9999 => $9, max => $10 };
        }
    }
    die "$f: table rows" unless keys %cy == @types && keys %nsx == @types;
    my ($cpu) = $s =~ /^result \(per-message latency, cpu (\d+)/m;
    push @lat, { file => $f, c => $c, omin => $omin, op50 => $op50, op99 => $op99, ghz => $ghz, zero => n($zero), cy => \%cy, ns => \%nsx, cpu => $cpu };
    push @all_common, [$f, $c];
}
sub mc {
    my $f = shift;
    my $s = slurp("$dir/$f");
    my $c = common($s, $f);
    push @all_common, [$f, $c];
    my @rows;
    for my $blk (split /\n(?=(?:demux|presplit), W = )/, $s) {
        next unless $blk =~ /^(demux|presplit), W = (\d+) workers \| (?:demux|main) cpu (\d+) \| worker cpus ([\d ]+) \|/;
        my ($mode, $w, $main, $wc) = ($1, $2, $3, $4);
        my ($secs, $msgs, $mps, $ns) = $blk =~ /timed ([\d.]+) s for ([\d,]+) messages \| aggregate ([\d.]+) M msgs\/s \| ([\d.]+) ns\/msg/ or die "$f W=$w";
        my ($lo, $hi, $ratio) = $blk =~ /messages per worker: min ([\d,]+) max ([\d,]+) \(max \/ mean ([\d.]+)\)/;
        my ($dg) = $blk =~ /book digest chain over \d+ chunk boundaries: ([0-9a-f]+)/;
        my ($chk) = $blk =~ /checks: (\w+)/;
        my @cd = $blk =~ /^  chunk +\d+ \|.*book digest ([0-9a-f]+)$/mg;
        my ($m) = $blk =~ /^  messages ([\d,]+) of /m;
        my ($drop) = $blk =~ /dropped_out_of_band ([\d,]+)/;
        push @rows, { mode => $mode, w => $w, main => $main, wc => [split ' ', $wc], secs => $secs, mps => $mps, ns => $ns,
                      lo => n($lo), hi => n($hi), ratio => $ratio, digest => $dg, checks => $chk, cd => \@cd,
                      messages => n($m), dropped => n($drop) };
    }
    return { file => $f, c => $c, rows => \@rows };
}
$demux = mc("perf_20190130_run_multicore_demux.log");
$presplit = mc("perf_20190130_run_multicore_presplit.log");
{
    my $f = "replay_20190130_differential.log";
    my $s = slurp("$dir/$f");
    my @cd = $s =~ /^  chunk +\d+ \| messages +[\d,]+ \| book digest ([0-9a-f]+)$/mg;
    my ($dg) = $s =~ /book digest chain over [\d,]+ chunk boundaries: ([0-9a-f]+)/;
    my ($mm) = $s =~ /orders compared in queue order [\d,]+ \| mismatches ([\d,]+)/;
    my ($cp) = $s =~ /^  checkpoints ([\d,]+) \| book comparisons/m;
    my ($rc) = $s =~ /^source: git commit (\S+)/m;
    $replay = { file => $f, digest => $dg, cd => \@cd, mismatches => n($mm), checkpoints => n($cp), commit => $rc // 'not recorded in the log', pass => ($s =~ /^RESULT: PASS$/m ? 1 : 0) };
}

# ---- consistency checks: every run reconstructed the same books ------------
my $ref = join ',', @{ $replay->{cd} };
die "replay log has no chunk digests" unless @{ $replay->{cd} };
my @digest_checks;
for my $e (@all_common) {
    my ($f, $c) = @$e;
    next if $f =~ /multicore/;
    die "$f: digest differs from the differential run" unless join(',', @{ $c->{chunk_digests} }) eq $ref && $c->{digest} eq $replay->{digest};
    die "$f: not PASS" unless $c->{pass};
    push @digest_checks, $f;
}
for my $m ($demux, $presplit) {
    for my $row (@{ $m->{rows} }) {
        die "$m->{file} W=$row->{w}: digest differs" unless join(',', @{ $row->{cd} }) eq $ref && $row->{digest} eq $replay->{digest};
        die "$m->{file} W=$row->{w}: checks $row->{checks}" unless $row->{checks} eq 'PASS';
    }
    die "$m->{file}: not PASS" unless $m->{c}{pass};
    push @digest_checks, $m->{file};
}
my %commits = map { $_->[1]{commit} => 1 } @all_common;
die "runs built from more than one commit: @{[keys %commits]}" unless keys %commits == 1;
my ($commit) = keys %commits;
my %power = map { $_->[1]{power} => 1 } @all_common;

# ---- assemble -------------------------------------------------------------
my $c0 = $thr[0]{c};
my @mps = map { $_->{mps} } @thr;
my @nsm = map { $_->{ns} } @thr;
my ($i_med) = grep { $thr[$_]{mps} == median(@mps) } 0 .. $#thr;

sub pct_obj { my $h = shift; obj(messages => $h->{n}, mean => $h->{mean}, min => $h->{min}, p50 => $h->{p50}, p90 => $h->{p90}, p99 => $h->{p99}, 'p99.9' => $h->{p999}, 'p99.99' => $h->{p9999}, max => $h->{max}) }
# headline latency run: the first run whose timer calibration clamped fewer
# than 1 in 10,000 samples to 0 (a calibration that reads high over-subtracts
# every sample); every run is reported either way.
my @valid = grep { $lat[$_]{zero} < $lat[$_]{cy}{all}{n} / 10000 } 0 .. $#lat;
my $i_lat = $valid[0] // 0;

my ($best_d) = sort { $b->{mps} <=> $a->{mps} } @{ $demux->{rows} };
my ($best_p) = sort { $b->{mps} <=> $a->{mps} } @{ $presplit->{rows} };
my $hl = $lat[$i_lat]{ns}{all};
my $headline = obj(
    single_core_msgs_per_s_millions => obj(median => median(@mps), min => (sort { $a <=> $b } @mps)[0], max => (sort { $a <=> $b } @mps)[-1], runs => scalar @mps),
    single_core_ns_per_msg_median => median(@nsm),
    per_message_latency_ns_all_messages => obj(run => "results/$lat[$i_lat]{file}", p50 => $hl->{p50}, p90 => $hl->{p90}, p99 => $hl->{p99}, 'p99.9' => $hl->{p999}, max => $hl->{max}),
    multi_core_best_demux => obj(workers => $best_d->{w}, aggregate_msgs_per_s_millions => $best_d->{mps}),
    multi_core_best_presplit => obj(workers => $best_p->{w}, aggregate_msgs_per_s_millions => $best_p->{mps}),
    books_identical_to_differential_run => 'yes, every run, at every chunk boundary',
);

# ---- placement comparison: pre-scan, first-add and causal ladders --------
# One binary per batch, the three placements interleaved back to back; every
# run must pass, run on AC power, and print the chunk digests of that day's
# causal differential run, which itself must pass with 0 mismatches.
sub lat_tables {
    my ($s, $f) = @_;
    my ($cyc_tab) = $s =~ /\n  TSC cycles\n(.*?)\n\n/s or die "$f cycles";
    my ($ns_tab) = $s =~ /\n  nanoseconds\n(.*?)\n\n/s or die "$f ns";
    my (%cy, %nsx);
    for my $pair ([\$cyc_tab, \%cy], [\$ns_tab, \%nsx]) {
        for my $line (split /\n/, ${$pair->[0]}) {
            next unless $line =~ /^  (\S+)\s+([\d,]+) \|\s+([\d.]+) \|\s+(\d+) \|\s+(\d+) \|\s+(\d+) \|\s+(\d+) \|\s+(\d+) \|\s+(\d+) \|\s+(\d+)$/;
            $pair->[1]{$1} = { n => n($2), mean => $3, min => $4, p50 => $5, p90 => $6, p99 => $7, p999 => $8, p9999 => $9, max => $10 };
        }
    }
    die "$f: table rows" unless keys %cy == @types && keys %nsx == @types;
    return (\%cy, \%nsx);
}
sub diff_run {
    my $f = shift;
    my $s = slurp("$dir/$f");
    my @cd = $s =~ /^  chunk +\d+ \| messages +[\d,]+ \| book digest ([0-9a-f]+)$/mg;
    my ($dg) = $s =~ /book digest chain over [\d,]+ chunk boundaries: ([0-9a-f]+)/;
    my ($cp, $cmm) = $s =~ /^  checkpoints ([\d,]+) \| book comparisons.*\| mismatches ([\d,]+)$/m or die "$f checkpoints";
    my ($tm, $tmm) = $s =~ /after every order message: ([\d,]+) messages checked.*\| mismatches ([\d,]+)$/m or die "$f touch";
    my ($lad, $ovf) = $s =~ /adds on the ladder ([\d,]+) \| in the overflow ([\d,]+)/ or die "$f adds";
    my ($drop, $unk) = $s =~ /dropped_out_of_band ([\d,]+) \| unknown order id ([\d,]+)/ or die "$f drops";
    my ($pl, $rc, $lv, $or) = $s =~ /causal ladders: placed ([\d,]+) \| re-centered ([\d,]+) times \| levels moved ([\d,]+) \| orders relinked ([\d,]+)/ or die "$f moves";
    my ($commit) = $s =~ /^source: git commit (\S+)/m;
    my $pass = $s =~ /^RESULT: PASS$/m ? 1 : 0;
    die "$f: not PASS or mismatches" unless $pass && n($cmm) == 0 && n($tmm) == 0 && n($drop) == 0 && n($unk) == 0;
    return { file => $f, cd => \@cd, digest => $dg, commit => $commit,
             json => obj(log => "results/$f", commit => $commit, result => 'PASS',
                         order_messages_checked_after_the_message => n($tm), mismatches_after_a_message => n($tmm),
                         checkpoints => n($cp), mismatches_at_a_checkpoint => n($cmm),
                         dropped_out_of_band => n($drop), unknown_order_ids => n($unk),
                         adds_on_a_ladder => n($lad), adds_in_the_overflow => n($ovf),
                         ladders_placed => n($pl), ladder_moves => n($rc), levels_moved => n($lv), orders_relinked => n($or),
                         digest_chain => $dg) };
}
sub placement_batch {
    my ($prefix, $diff, $n_lat) = @_;
    my %P;
    my %commits;
    my $want = join ',', @{ $diff->{cd} };
    for my $pl ('prescan', 'firstadd', 'causal') {
        my (@t, @l);
        for my $i (1 .. 5) {
            my $f = "${prefix}_${pl}_throughput_$i.log";
            my $s = slurp("$dir/$f");
            my $c = common($s, $f);
            my ($secs, $mps, $ns) = $s =~ /timed dispatch ([\d.]+) s for [\d,]+ messages \| ([\d.]+) M msgs\/s \| ([\d.]+) ns\/msg/ or die "$f: result";
            my ($lad, $ovf) = $s =~ /adds on a ladder ([\d,]+) \| adds in the overflow ([\d,]+)/ or die "$f: adds";
            my @mv = $s =~ /causal ladders: placed ([\d,]+) \| re-centered ([\d,]+) times \| levels moved ([\d,]+) \| orders relinked ([\d,]+)/;
            die "$f: not PASS" unless $c->{pass};
            die "$f: not on AC ($c->{power})" unless $c->{power} =~ /^AC line online/;
            die "$f: digests differ from $diff->{file}" unless join(',', @{ $c->{chunk_digests} }) eq $want && $c->{digest} eq $diff->{digest};
            $commits{ $c->{commit} } = 1;
            push @t, { file => $f, secs => $secs, mps => $mps, ns => $ns, lad => n($lad), ovf => n($ovf), mv => [map { n($_) } @mv] };
        }
        for my $i (1 .. $n_lat) {
            my $f = "${prefix}_${pl}_latency_$i.log";
            my $s = slurp("$dir/$f");
            my $c = common($s, $f);
            die "$f: not PASS" unless $c->{pass};
            die "$f: not on AC ($c->{power})" unless $c->{power} =~ /^AC line online/;
            die "$f: digests differ from $diff->{file}" unless join(',', @{ $c->{chunk_digests} }) eq $want && $c->{digest} eq $diff->{digest};
            $commits{ $c->{commit} } = 1;
            my ($cy, $nsx) = lat_tables($s, $f);
            my ($ovh) = $s =~ /subtracted from every sample, clamped at 0: (\d+) cycles/ or die "$f ovh";
            my ($zero) = $s =~ /samples clamped to 0 after overhead subtraction: ([\d,]+)/;
            my @mvl = $s =~ /placed or moved a ladder \(included in the rows above\): ([\d,]+) \| p50 (\d+) \| p90 (\d+) \| p99 (\d+) \| max (\d+) ns/;
            push @l, { file => $f, ovh => $ovh, zero => n($zero), ns => $nsx, mvl => \@mvl };
        }
        my @m = map { $_->{mps} } @t;
        my @nsv = map { $_->{ns} } @t;
        my @srt = sort { $a <=> $b } @m;
        $P{$pl} = { t => \@t, l => \@l, med => median(@m), min => $srt[0], max => $srt[-1], nsmed => median(@nsv) };
    }
    die "$prefix: runs built from more than one commit" unless keys %commits == 1;
    my ($commit) = keys %commits;
    my $pobj = sub {
        my $pl = shift;
        my $p = $P{$pl};
        my $t0 = $p->{t}[0];
        my @o = (
            throughput_runs => [map { obj(log => "results/$_->{file}", timed_s => $_->{secs}, msgs_per_s_millions => $_->{mps}, ns_per_msg => $_->{ns}) } @{ $p->{t} }],
            msgs_per_s_millions => obj(min => $p->{min}, median => $p->{med}, max => $p->{max}),
            ns_per_msg_median => $p->{nsmed},
            adds_on_a_ladder => $t0->{lad},
            adds_in_the_overflow => $t0->{ovf},
        );
        push @o, (ladders_placed => $t0->{mv}[0], ladder_moves => $t0->{mv}[1], levels_moved => $t0->{mv}[2], orders_relinked => $t0->{mv}[3]) if @{ $t0->{mv} };
        push @o, (latency_runs => [map { my $l = $_; obj(
            log => "results/$l->{file}",
            timer_overhead_subtracted_cycles => $l->{ovh},
            samples_clamped_to_zero => $l->{zero},
            ns_all_messages => pct_obj($l->{ns}{all}),
            ns_by_type => obj(map { ($_ => pct_obj($l->{ns}{$_})) } @types),
            (@{ $l->{mvl} } ? (ns_messages_that_placed_or_moved_a_ladder => obj(messages => n($l->{mvl}[0]), p50 => $l->{mvl}[1], p90 => $l->{mvl}[2], p99 => $l->{mvl}[3], max => $l->{mvl}[4])) : ()),
        ) } @{ $p->{l} }]);
        return obj(@o);
    };
    return { P => \%P, commit => $commit, json => obj(
        commit => $commit,
        differential => $diff->{json},
        prescan => $pobj->('prescan'),
        first_add => $pobj->('firstadd'),
        causal => $pobj->('causal'),
        causal_over_prescan_median_throughput => r($P{causal}{med} / $P{prescan}{med}, 3),
        causal_over_first_add_median_throughput => r($P{causal}{med} / $P{firstadd}{med}, 3),
    ) };
}
my $diff1 = diff_run('replay_20190130_causal_differential.log');
die "causal and pre-scan differential runs printed different chunk digests"
    unless join(',', @{ $diff1->{cd} }) eq join(',', @{ $replay->{cd} }) && $diff1->{digest} eq $replay->{digest};
my $pc1 = placement_batch('perf_20190130_causal', $diff1, 2);
my $diff2 = diff_run('replay_20191230_causal_differential.log');
my $pc2 = placement_batch('perf_20191230_causal', $diff2, 1);
my $cz = $pc1->{P}{causal};
my $pz = $pc1->{P}{prescan};
my $fz = $pc1->{P}{firstadd};
my $cl = $cz->{l}[0]{ns}{all};
my $pl1 = $pz->{l}[0]{ns}{all};
$headline = obj(
    placement => 'causal: each book places and moves its own ladder from the messages it has already applied (--placement causal)',
    single_core_msgs_per_s_millions => obj(median => $cz->{med}, min => $cz->{min}, max => $cz->{max}, runs => scalar @{ $cz->{t} }),
    single_core_ns_per_msg_median => $cz->{nsmed},
    per_message_latency_ns_all_messages => obj(run => "results/$cz->{l}[0]{file}", p50 => $cl->{p50}, p90 => $cl->{p90}, p99 => $cl->{p99}, 'p99.9' => $cl->{p999}, 'p99.99' => $cl->{p9999}, max => $cl->{max}),
    upper_bound_prescan_placement => obj(
        note => 'ladders sized and placed from a pre-scan of the same day\'s file (look-ahead); same binary, same batch',
        msgs_per_s_millions => obj(median => $pz->{med}, min => $pz->{min}, max => $pz->{max}),
        per_message_latency_ns_all_messages => obj(run => "results/$pz->{l}[0]{file}", p50 => $pl1->{p50}, p90 => $pl1->{p90}, p99 => $pl1->{p99}, 'p99.9' => $pl1->{p999}),
    ),
    first_add_placement_msgs_per_s_millions_median => $fz->{med},
    second_day_20191230_msgs_per_s_millions_median => obj(causal => $pc2->{P}{causal}{med}, prescan => $pc2->{P}{prescan}{med}, first_add => $pc2->{P}{firstadd}{med}),
    multi_core_best_demux => obj(workers => $best_d->{w}, aggregate_msgs_per_s_millions => $best_d->{mps}, placement => 'prescan', commit => $commit),
    multi_core_best_presplit => obj(workers => $best_p->{w}, aggregate_msgs_per_s_millions => $best_p->{mps}, placement => 'prescan', commit => $commit),
    books_identical_to_differential_run => 'yes, every run, at every chunk boundary',
);
my $placement_json = obj(
    what => 'the same binary with three ladder placements, run back to back and interleaved on logical CPU 1 (P-core), on AC power: prescan (sizes and windows from a pre-scan of the same file: look-ahead, an upper bound), first-add (fixed 2,048-tick ladders centered on each symbol\'s first add), causal (2,048-tick ladders each book centers on its first add and re-centers on its midpoint after 8 near-touch misses; the moves run inside the timed loop). Pool and id-map capacities come from the pre-scan in all three',
    policy_parameters => obj(
        ladder_ticks => 'the widest power of two whose ladders fit the 1,024 MB budget across all books: 2,048 on both days',
        near_touch_misses_before_a_move => 8,
        grid => '$0.01 for a center at or above $1.00, $0.0001 below',
        chosen => 'before any causal run on either day; not fitted to either day',
    ),
    day_20190130 => $pc1->{json},
    day_20191230 => obj(
        data => obj(file => '12302019.NASDAQ_ITCH50', source => 'https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/', bytes => 8251407909,
                    sha256 => '5d81c2e14a0f748b29c674b6a342796932702034b4dd341e39e9a9ec5bac610f',
                    gz_sha256 => 'ef03df46a27e6bda4dead017f84c2e3979df7211f02c7868b51d53fceb99c689'),
        (map { @$_ } @{ $pc2->{json} }),
    ),
    machine_state => 'AC line online before and after every run; the battery charged from 27% to 99% over the two batches, which ran back to back (power state and busiest processes before each run in the env logs)',
    env_logs => ['results/perf_20190130_causal_env.log', 'results/perf_20191230_causal_env.log'],
);

if (($ARGV[0] // '') eq '20191230') {
    print js(obj(
        note => 'second NASDAQ sample day, measured only for the ladder placement comparison; the README headline day is 2019-01-30 (perf_20190130.json)',
        machine => obj(cpu => $c0->{cpu}, os => $c0->{os} . ' (Windows 11)', compiler => $c0->{compiler}),
        policy_parameters => $placement_json->[1][1],
        what => $placement_json->[0][1],
        day_20191230 => $placement_json->[3][1],
        env_log => 'results/perf_20191230_causal_env.log',
    )), "
";
    exit 0;
}

my $json = obj(
    headline => $headline,
    placement_comparison => $placement_json,
    data => obj(
        file => '01302019.NASDAQ_ITCH50 (decompressed NASDAQ TotalView-ITCH 5.0 BinaryFILE)',
        date => '2019-01-30',
        source => 'https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/',
        bytes => 11245883092,
        sha256 => '1d0972ffc25b35902ccc3f9069aae517da56903d5795f872902b8697315f30c3',
        messages => $c0->{messages},
        manifest => 'data/MANIFEST.md',
    ),
    machine => obj(
        cpu => $c0->{cpu},
        logical_processors => $c0->{logical},
        cores => "16 cores, no SMT: 6 P-cores, 8 E-cores, 2 low-power E-cores",
        core_types => 'from GetSystemCpuSetInformation: efficiency class 1 (P-cores) on logical CPUs 0, 1, 10-13; class 0 on 2-9 (E-cores) and 14-15 (low-power E-cores, separate last-level cache)',
        os => $c0->{os} . ' (Windows 11)',
        power => (keys %power == 1 ? (keys %power)[0] : join(' / ', sort keys %power)),
        power_plan => 'Balanced (powercfg /getactivescheme, recorded before every run in perf_20190130_run_env.log)',
        machine_state => 'laptop with the owner\'s usual applications open (browser, music player); per-process CPU use in the 5 s before each run is in perf_20190130_run_env.log',
    ),
    build => obj(
        compiler => $c0->{compiler},
        flags => $c0->{flags},
        latency_build_flags => $lat[0]{c}{flags},
        git_commit => $commit,
        binaries => 'lob_perf (src/perf_main.cpp) for throughput and multi-core and lob_perf_latency (the same source with -DLOB_PERF_LATENCY) for per-message latency, both built from git_commit; lob_replay (src/replay_main.cpp) for the differential run, built from correctness.differential_commit',
    ),
    tsc => obj(
        invariant => $c0->{invariant},
        cpuid_15h_nominal_ghz => $c0->{tsc_cpuid},
        method => 'rdtsc against std::chrono::steady_clock (QueryPerformanceCounter): five 1 s busy windows before each pass, and again over the whole timed pass; latency cycles are converted with the whole-pass value printed in the same log',
        measured_ghz_per_run => obj(map { ($_->[0] => obj(windows => [split ' ', $_->[1]{tsc_windows}], median => $_->[1]{tsc_median}, whole_pass => $_->[1]{tsc_pass})) } grep { defined $_->[1]{tsc_pass} } @all_common),
    ),
    correctness => obj(
        note => 'every run below printed a digest of every book\'s full state (levels and queue order) after each of the 42 chunks; all of them equal the digests of the differential run, which compared every book against the reference model',
        differential_log => "results/$replay->{file}",
        differential_commit => $replay->{commit},
        differential_checkpoints => $replay->{checkpoints},
        differential_mismatches => $replay->{mismatches},
        digest_chain => $replay->{digest},
        runs_matching_every_chunk_digest => [map { "results/$_" } @digest_checks],
        dropped_out_of_band => 0,
        unknown_order_ids => 0,
        bad_length => 0,
    ),
    single_core_throughput => obj(
        placement => "prescan (ladders sized and placed from a pre-scan of the same file: look-ahead)",
        what => 'end to end over the full day: parse, validate and apply every message (dispatch_segment: frame, look up the stock_locate book, itch::dispatch_checked<WireUnits>); only that loop over a chunk already in memory is timed, summed over the 42 chunks of 256 MB; file reads, the pre-scan and the per-chunk digests are not timed',
        cpu => 'logical CPU ' . $thr[0]{cpu} . ' (P-core), thread pinned, process HIGH_PRIORITY_CLASS',
        runs => [map { obj(log => "results/$_->{file}", timed_s => $_->{secs}, msgs_per_s_millions => $_->{mps}, ns_per_msg => $_->{ns}, tsc_cycles_per_msg => $_->{cyc}) } @thr],
        msgs_per_s_millions => obj(min => (sort { $a <=> $b } @mps)[0], median => median(@mps), max => (sort { $a <=> $b } @mps)[-1]),
        ns_per_msg => obj(min => (sort { $a <=> $b } @nsm)[0], median => median(@nsm), max => (sort { $a <=> $b } @nsm)[-1]),
        e_core_comparison => obj(log => "results/$ecore->{file}", cpu => "logical CPU $ecore->{cpu} (E-core)", msgs_per_s_millions => $ecore->{mps}, ns_per_msg => $ecore->{ns}, runs => 1),
    ),
    per_message_latency => obj(
        placement => "prescan",
        what => 'every message of the day timed individually (no sampling): lfence; rdtsc; book lookup + itch::dispatch_checked<WireUnits>; rdtscp; lfence. The minimum of 100,000 empty timer pairs is subtracted from each sample (clamped at 0). Exact histogram, nearest-rank percentiles. Serialized timing measures one message in isolation; it is not the throughput figure',
        cpu => 'logical CPU ' . $lat[0]{cpu} . ' (P-core), thread pinned, process HIGH_PRIORITY_CLASS',
        headline_run => "results/$lat[$i_lat]{file}",
        headline_rule => "the first run in which fewer than 1 in 10,000 samples were clamped to 0 by the overhead subtraction; a run whose empty-timer calibration read high (run 2: minimum 76 cycles against 38 in runs 1 and 3) subtracts too much from every sample and is reported but not used as the headline",
        valid_runs => [map { "results/$lat[$_]{file}" } @valid],
        runs => [map { my $l = $_; obj(
            log => "results/$l->{file}",
            tsc_ghz => $l->{ghz},
            timer_overhead_cycles => obj(min_subtracted => $l->{omin}, p50 => $l->{op50}, p99 => $l->{op99}),
            samples_clamped_to_zero => $l->{zero},
            cycles => obj(map { ($_ => pct_obj($l->{cy}{$_})) } @types),
            ns => obj(map { ($_ => pct_obj($l->{ns}{$_})) } @types),
        ) } @lat],
    ),
    multi_core => obj(
        placement => "prescan",
        note => 'shard = stock_locate % W, as in include/lob/engine.hpp. demux: one thread reads each chunk and routes every message through W SPSC rings (BasicParallelEngine<ExactOrderBook, WireUnits>), timed from the first push of a chunk until every ring is drained. presplit: each chunk is first split per shard outside the timed region, then W workers apply their own buffer in parallel, timed from the start signal until the last worker finishes. The two are different measurements: presplit leaves out the demux entirely',
        cpu_assignment => 'main/demux thread on logical CPU 0; workers on 1, 10, 11, 12, 13 (P-cores), then 2-9 (E-cores)',
        demux => [map { obj(workers => $_->{w}, demux_cpu => $_->{main}, worker_cpus => $_->{wc}, timed_s => $_->{secs}, aggregate_msgs_per_s_millions => $_->{mps}, ns_per_msg => $_->{ns}, max_worker_share_over_mean => $_->{ratio}) } @{ $demux->{rows} }],
        demux_log => "results/$demux->{file}",
        presplit => [map { obj(workers => $_->{w}, main_cpu => $_->{main}, worker_cpus => $_->{wc}, timed_s => $_->{secs}, aggregate_msgs_per_s_millions => $_->{mps}, ns_per_msg => $_->{ns}, max_worker_share_over_mean => $_->{ratio}) } @{ $presplit->{rows} }],
        presplit_log => "results/$presplit->{file}",
    ),
    not_measured => [
        'memory use and page faults',
        'core clock frequency during the runs (the TSC runs at a constant rate; turbo state is not recorded)',
        'any host other than this laptop; no Linux run',
        'network or file-read time: every figure is for messages already in memory',
        'multi-core runs were made once each, not repeated',
    ],
);
print js($json), "\n";
