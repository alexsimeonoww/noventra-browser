<script lang="ts">
    let { height = "64px" }: { height?: string } = $props();
</script>

<div class="noventra-logo" style:height style:width={height} role="img" aria-label="Noventra"></div>

<style>
    .noventra-logo {
        flex-shrink: 0;
        background-color: contrast-color(var(--color-primary-background, var(--bg-gradient-start)));
        mask: url(chrome://theme/IDR_PRODUCT_LOGO@2x) center / contain no-repeat;
    }
</style>
