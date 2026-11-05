while (my $l = <STDIN>) {
    chomp $l; my ($v, $c, $s, $hex) = split / /, $l;
    $hex //= ""; my $pw = pack("H*", $hex);
    my $h = crypt($pw, "\$$v\$$c\$$s");
    print "$v $c $s $hex ", (defined $h ? $h : "undef"), "\n";
}
